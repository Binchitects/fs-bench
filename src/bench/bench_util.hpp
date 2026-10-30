// Shared machinery for benchmarks: a multi-threaded I/O engine with latency capture.
#pragma once

#include <algorithm>
#include <atomic>
#include <latch>
#include <thread>
#include <vector>

#include "framework.hpp"

namespace fsb::bu {

using namespace fsb::plat::open_flags;
using plat::File;

enum class Pattern { SeqRead, SeqWrite, RandRead, RandWrite, Mixed };

struct IoSpec {
    Pattern pattern = Pattern::SeqRead;
    std::vector<fs::path> files;  // one shared file, or one file per thread
    uint64_t span = 0;            // bytes per file (seq: total region; rand: offset range)
    size_t bs = 1 << 20;
    bool direct = false;
    unsigned threads = 1;
    double duration_s = 0;        // random patterns: run time; seq: ignored (one pass)
    int read_pct = 70;            // Mixed only
    bool end_fsync = true;        // include a final fsync of written files in the timing
    bool drop_cache_first = true; // buffered reads: evict the file from the page cache first
};

inline std::string mode_name(bool direct) { return direct ? "direct" : "buffered"; }

inline std::string size_name(uint64_t b) {
    if (b >= (1ull << 30) && b % (1ull << 30) == 0) return cat(b >> 30, "G");
    if (b >= (1ull << 20) && b % (1ull << 20) == 0) return cat(b >> 20, "M");
    if (b >= 1024 && b % 1024 == 0) return cat(b >> 10, "K");
    return std::to_string(b);
}

// Runs fn(t) on n threads that start simultaneously; returns wall time in ns.
template <typename F>
uint64_t run_threads(unsigned n, F fn) {
    std::latch start(static_cast<std::ptrdiff_t>(n) + 1);
    std::vector<std::thread> ts;
    std::vector<std::exception_ptr> errs(n);
    for (unsigned i = 0; i < n; ++i)
        ts.emplace_back([&, i] {
            start.arrive_and_wait();
            try {
                fn(i);
            } catch (...) {
                errs[i] = std::current_exception();
            }
        });
    Timer t;
    start.arrive_and_wait();
    t.reset();
    for (auto& th : ts) th.join();
    uint64_t ns = t.elapsed_ns();
    for (auto& e : errs)
        if (e) std::rethrow_exception(e);
    return ns;
}

inline BenchResult run_io(const IoSpec& s) {
    const bool writes = s.pattern == Pattern::SeqWrite || s.pattern == Pattern::RandWrite || s.pattern == Pattern::Mixed;
    const bool seq = s.pattern == Pattern::SeqRead || s.pattern == Pattern::SeqWrite;
    const bool per_thread_file = s.files.size() > 1;
    if (!writes && !s.direct && s.drop_cache_first)
        for (auto& f : s.files) plat::drop_file_cache(f);

    unsigned flags = read | (writes ? write : 0u) | (s.direct ? direct : 0u) |
                     (seq ? sequential : plat::open_flags::random);
    const uint64_t blocks = s.span / s.bs;
    std::vector<LatencyRecorder> lat(s.threads);
    std::vector<uint64_t> ops(s.threads, 0), bytes(s.threads, 0);
    std::vector<File> handles(s.threads);
    for (unsigned t = 0; t < s.threads; ++t) handles[t] = File::open(s.files[per_thread_file ? t : 0], flags);

    uint64_t ns = run_threads(s.threads, [&](unsigned t) {
        File& f = handles[t];
        AlignedBuffer buf(s.bs);
        Rng rng(0xB00Cull * (t + 1));
        rng.fill(buf.data(), buf.size());
        auto& rec = lat[t];
        rec.reserve(seq ? static_cast<size_t>(blocks / (per_thread_file ? 1 : s.threads) + 1) : 1 << 20);
        auto do_op = [&](bool is_read, uint64_t off) {
            uint64_t t0 = now_ns();
            if (is_read) {
                f.pread(buf.data(), s.bs, off);
            } else {
                std::memcpy(buf.data(), &off, sizeof(off));  // make every block unique (defeats dedup)
                f.pwrite(buf.data(), s.bs, off);
            }
            rec.add(now_ns() - t0);
            ops[t]++;
            bytes[t] += s.bs;
        };
        if (seq) {
            uint64_t first = per_thread_file ? 0 : blocks * t / s.threads;
            uint64_t last = per_thread_file ? blocks : blocks * (t + 1) / s.threads;
            for (uint64_t b = first; b < last; ++b) do_op(s.pattern == Pattern::SeqRead, b * s.bs);
        } else {
            const uint64_t deadline = now_ns() + static_cast<uint64_t>(s.duration_s * 1e9);
            while (now_ns() < deadline) {
                bool is_read = s.pattern == Pattern::RandRead ||
                               (s.pattern == Pattern::Mixed && static_cast<int>(rng.below(100)) < s.read_pct);
                do_op(is_read, rng.below(blocks) * s.bs);
            }
        }
    });
    uint64_t sync_ns = 0;
    if (writes && s.end_fsync) {
        Timer t;
        if (per_thread_file)
            for (auto& h : handles) h.sync();
        else
            handles[0].sync();
        sync_ns = t.elapsed_ns();
    }
    BenchResult r;
    r.seconds = static_cast<double>(ns + sync_ns) / 1e9;
    for (unsigned t = 0; t < s.threads; ++t) {
        r.ops += ops[t];
        r.bytes += bytes[t];
        if (t) lat[0].merge(lat[t]);
    }
    r.has_latency = true;
    r.lat = lat[0].summarize();
    if (sync_ns) r.note = cat("incl. final fsync ", fmt_duration_ns(static_cast<double>(sync_ns)));
    return r;
}

// Fills a file with incompressible data (not timed).
inline void fill_file(const fs::path& p, uint64_t size, uint64_t seed) {
    File f = File::open(p, write | create | truncate | sequential);
    AlignedBuffer buf(4 << 20);
    Rng r(seed);
    for (uint64_t off = 0; off < size; off += buf.size()) {
        size_t n = static_cast<size_t>(std::min<uint64_t>(buf.size(), size - off));
        r.fill(buf.data(), n);
        f.pwrite(buf.data(), n, off);
    }
    f.sync();
}

inline std::vector<bool> io_modes(BenchContext& ctx) {
    std::vector<bool> out;
    for (auto& m : ctx.cfg.io_modes) {
        if (m == "buffered") out.push_back(false);
        if (m == "direct") {
            if (ctx.direct_io_supported()) out.push_back(true);
            else ctx.feature("Direct I/O (bench)", "unsupported, skipped");
        }
    }
    return out;
}

// Data span for throughput tests, rounded down to a multiple of `bs`.
inline uint64_t span_for(const BenchContext& ctx, size_t bs) { return std::max<uint64_t>(ctx.cfg.file_size / bs, 1) * bs; }

}  // namespace fsb::bu
