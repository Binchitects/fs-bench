// Data-path benchmarks: sequential / random / mixed I/O, thread scaling, mmap, sync latency.
#include <cstring>

#include "bench_util.hpp"

namespace fsb {
using namespace bu;

namespace {

void emit(BenchContext& ctx, BenchResult r, const char* name, const std::string& params) {
    r.category = "io";
    r.name = name;
    r.params = params;
    ctx.report(std::move(r));
}

std::string p(size_t bs, bool direct, unsigned threads) {
    return cat("bs=", size_name(bs), " ", mode_name(direct), " T=", threads);
}

}  // namespace

FSB_BENCH(io, seq_write, "sequential write of the data size (single stream, final fsync included)") {
    for (bool direct : io_modes(ctx))
        for (size_t bs : ctx.cfg.block_sizes) {
            fs::path f = ctx.path("seq_write.bin");
            { File::open(f, write | create | truncate); }
            IoSpec s;
            s.pattern = Pattern::SeqWrite;
            s.files = {f};
            s.bs = bs;
            s.span = span_for(ctx, bs);
            s.direct = direct;
            emit(ctx, run_io(s), "seq_write", p(bs, direct, 1));
            fs::remove(f);
        }
}

FSB_BENCH(io, seq_overwrite, "sequential rewrite of an existing, fully allocated file (no allocation cost)") {
    fs::path f = ctx.shared_data_file(ctx.cfg.file_size, "rw");
    for (bool direct : io_modes(ctx)) {
        IoSpec s;
        s.pattern = Pattern::SeqWrite;
        s.files = {f};
        s.bs = 1 << 20;
        s.span = span_for(ctx, s.bs);
        s.direct = direct;
        emit(ctx, run_io(s), "seq_overwrite", p(s.bs, direct, 1));
    }
}

FSB_BENCH(io, seq_read, "sequential read of the data file with cold cache") {
    fs::path f = ctx.shared_data_file(ctx.cfg.file_size);
    for (bool direct : io_modes(ctx))
        for (size_t bs : ctx.cfg.block_sizes) {
            IoSpec s;
            s.pattern = Pattern::SeqRead;
            s.files = {f};
            s.bs = bs;
            s.span = span_for(ctx, bs);
            s.direct = direct;
            auto r = run_io(s);
            if (!direct) r.note = "cache dropped before run";
            emit(ctx, r, "seq_read", p(bs, direct, 1));
        }
}

FSB_BENCH(io, seq_read_cached, "sequential read served from the page cache (filesystem CPU overhead)") {
    fs::path f = ctx.shared_data_file(ctx.cfg.file_size);
    IoSpec s;
    s.pattern = Pattern::SeqRead;
    s.files = {f};
    s.bs = 1 << 20;
    s.span = std::min<uint64_t>(span_for(ctx, s.bs), ctx.sys.ram_bytes / 4);
    s.drop_cache_first = false;
    run_io(s);  // warm the cache
    auto r = run_io(s);
    r.note = "warm cache";
    emit(ctx, r, "seq_read_cached", p(s.bs, false, 1));
}

FSB_BENCH(io, seq_write_mt, "parallel sequential writers, one file each (aggregate throughput)") {
    for (bool direct : io_modes(ctx))
        for (unsigned t : ctx.cfg.threads) {
            if (t < 2) continue;
            IoSpec s;
            s.pattern = Pattern::SeqWrite;
            s.threads = t;
            s.bs = 1 << 20;
            s.span = std::max<uint64_t>(ctx.cfg.file_size / t / s.bs, 1) * s.bs;
            s.direct = direct;
            for (unsigned i = 0; i < t; ++i) {
                s.files.push_back(ctx.path(cat("mt_", i, ".bin")));
                File::open(s.files.back(), write | create | truncate);
            }
            emit(ctx, run_io(s), "seq_write_mt", p(s.bs, direct, t));
            for (auto& f : s.files) fs::remove(f);
        }
}

FSB_BENCH(io, seq_read_mt, "parallel sequential readers over disjoint regions of one file") {
    fs::path f = ctx.shared_data_file(ctx.cfg.file_size);
    for (bool direct : io_modes(ctx))
        for (unsigned t : ctx.cfg.threads) {
            if (t < 2) continue;
            IoSpec s;
            s.pattern = Pattern::SeqRead;
            s.files = {f};
            s.threads = t;
            s.bs = 1 << 20;
            s.span = span_for(ctx, s.bs);
            s.direct = direct;
            emit(ctx, run_io(s), "seq_read_mt", p(s.bs, direct, t));
        }
}

FSB_BENCH(io, rand_read, "random reads, scaling block size and thread count (IOPS + latency)") {
    fs::path f = ctx.shared_data_file(ctx.cfg.file_size);
    for (bool direct : io_modes(ctx))
        for (size_t bs : ctx.cfg.rand_block_sizes)
            for (unsigned t : ctx.cfg.threads) {
                IoSpec s;
                s.pattern = Pattern::RandRead;
                s.files = {f};
                s.threads = t;
                s.bs = bs;
                s.span = span_for(ctx, bs);
                s.direct = direct;
                s.duration_s = ctx.cfg.duration_s;
                auto r = run_io(s);
                if (!direct) r.note = "page cache warms during run";
                emit(ctx, r, "rand_read", p(bs, direct, t));
            }
}

FSB_BENCH(io, rand_write, "random overwrites of an allocated file (final fsync included)") {
    fs::path f = ctx.shared_data_file(ctx.cfg.file_size, "rw");
    for (bool direct : io_modes(ctx))
        for (size_t bs : ctx.cfg.rand_block_sizes)
            for (unsigned t : ctx.cfg.threads) {
                IoSpec s;
                s.pattern = Pattern::RandWrite;
                s.files = {f};
                s.threads = t;
                s.bs = bs;
                s.span = span_for(ctx, bs);
                s.direct = direct;
                s.duration_s = ctx.cfg.duration_s;
                emit(ctx, run_io(s), "rand_write", p(bs, direct, t));
            }
}

FSB_BENCH(io, rand_mixed, "random 70% read / 30% write mix (OLTP-like)") {
    fs::path f = ctx.shared_data_file(ctx.cfg.file_size, "rw");
    for (bool direct : io_modes(ctx))
        for (unsigned t : ctx.cfg.threads) {
            IoSpec s;
            s.pattern = Pattern::Mixed;
            s.files = {f};
            s.threads = t;
            s.bs = ctx.cfg.rand_block_sizes.front();
            s.span = span_for(ctx, s.bs);
            s.direct = direct;
            s.duration_s = ctx.cfg.duration_s;
            s.read_pct = 70;
            emit(ctx, run_io(s), "rand_mixed", p(s.bs, direct, t) + " r/w=70/30");
        }
}

FSB_BENCH(io, mmap_read, "sequential read through a memory mapping (cold cache)") {
    fs::path f = ctx.shared_data_file(ctx.cfg.file_size);
    plat::drop_file_cache(f);
    File h = File::open(f, read);
    const uint64_t size = h.size();
    const uint64_t window = 256ull << 20;
    const size_t chunk = 1 << 20;
    volatile uint64_t sink = 0;  // keeps the loads observable so they are not optimized away
    LatencyRecorder rec;
    Timer t;
    for (uint64_t off = 0; off < size; off += window) {
        size_t len = static_cast<size_t>(std::min(window, size - off));
        auto m = h.map(off, len, false);
        for (size_t i = 0; i < len; i += chunk) {
            uint64_t t0 = now_ns();
            uint64_t acc = 0;
            const size_t end = std::min(i + chunk, len);
            for (size_t j = i; j + 8 <= end; j += 64) {  // one load per cache line
                uint64_t v;
                std::memcpy(&v, m.data() + j, 8);
                acc ^= v;
            }
            sink = sink ^ acc;
            rec.add(now_ns() - t0);
        }
    }
    BenchResult r;
    r.seconds = t.elapsed_s();
    r.bytes = size;
    r.ops = rec.size();
    r.has_latency = true;
    r.lat = rec.summarize();
    r.note = "1 MiB per op, every cache line touched";
    emit(ctx, r, "mmap_read", "window=256M");
}

// ---------------------------------------------------------------- durability latency

FSB_BENCH(latency, sync_write, "latency of durable 4 KiB writes: fsync, fdatasync, O_DSYNC, direct+O_DSYNC") {
    struct Variant {
        const char* name;
        unsigned flags;
        bool append;
        int sync;  // 0 none (flag-based), 1 fsync, 2 fdatasync
    };
    std::vector<Variant> vs = {
        {"append+fsync", write | create | truncate, true, 1},
        {"append+fdatasync", write | create | truncate, true, 2},
        {"overwrite+fdatasync", write | create, false, 2},
        {"overwrite O_DSYNC", write | create | dsync, false, 0},
    };
    if (ctx.direct_io_supported()) vs.push_back({"overwrite direct+O_DSYNC", write | create | direct | dsync, false, 0});
    const size_t bs = 4096;
    const uint64_t prealloc = 64ull << 20;
    const double dur = std::min(ctx.cfg.duration_s, 5.0);
    for (auto& v : vs) {
        fs::path p = ctx.path("wal.bin");
        if (!v.append) fill_file(p, prealloc, 7);  // overwrite pattern: blocks already allocated
        File f = File::open(p, v.flags);
        AlignedBuffer buf(bs);
        ctx.rng.fill(buf.data(), bs);
        LatencyRecorder rec;
        uint64_t off = 0;
        Timer t;
        const uint64_t deadline = now_ns() + static_cast<uint64_t>(dur * 1e9);
        while (now_ns() < deadline && rec.size() < 200000) {
            uint64_t t0 = now_ns();
            f.pwrite(buf.data(), bs, off);
            if (v.sync == 1) f.sync();
            if (v.sync == 2) f.datasync();
            rec.add(now_ns() - t0);
            off = v.append ? off + bs : (off + bs) % prealloc;
        }
        BenchResult r;
        r.seconds = t.elapsed_s();
        r.ops = rec.size();
        r.has_latency = true;
        r.lat = rec.summarize();
        r.category = "latency";
        r.name = "sync_write";
        r.params = cat("4K ", v.name);
        ctx.report(r);
        f.close();
        fs::remove(p);
    }
}

}  // namespace fsb
