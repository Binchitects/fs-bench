// Concurrent access: parallel writers, atomic append, create/mkdir/rename races.
#include <atomic>
#include <barrier>
#include <cstring>
#include <thread>

#include "test_util.hpp"

namespace fsb {
using namespace tu;

namespace {

unsigned worker_count() { return std::clamp(std::thread::hardware_concurrency(), 4u, 16u); }

// Runs fn(i) on n threads, rethrowing the first exception.
template <typename F>
void parallel(unsigned n, F fn) {
    std::vector<std::thread> ts;
    std::vector<std::exception_ptr> errs(n);
    for (unsigned i = 0; i < n; ++i)
        ts.emplace_back([&, i] {
            try {
                fn(i);
            } catch (...) {
                errs[i] = std::current_exception();
            }
        });
    for (auto& t : ts) t.join();
    for (auto& e : errs)
        if (e) std::rethrow_exception(e);
}

}  // namespace

FSB_TEST(concurrency, parallel_files, "threads writing separate files concurrently all verify") {
    const unsigned n = worker_count();
    const uint64_t seed = ctx.rng.next();
    parallel(n, [&](unsigned t) {
        std::vector<unsigned char> buf(256 << 10);
        for (int i = 0; i < 20; ++i) {
            pattern_fill(buf.data(), buf.size(), seed + t * 1000 + i, 0);
            File f = File::open(ctx.path(cat("t", t, "_", i)), write | create | truncate);
            f.pwrite(buf.data(), buf.size(), 0);
        }
    });
    std::vector<unsigned char> want(256 << 10);
    for (unsigned t = 0; t < n; ++t)
        for (int i = 0; i < 20; ++i) {
            pattern_fill(want.data(), want.size(), seed + t * 1000 + i, 0);
            auto got = read_file(ctx.path(cat("t", t, "_", i)));
            CHECK_MSG(got.size() == want.size() && std::memcmp(got.data(), want.data(), want.size()) == 0,
                      "file t", t, "_", i, " corrupted");
        }
}

FSB_TEST(concurrency, disjoint_pwrite, "threads writing interleaved stripes of one file produce the exact image") {
    const unsigned n = worker_count();
    const size_t stripe = 64 << 10;
    const size_t stripes = 1024;  // 64 MiB
    const uint64_t seed = ctx.rng.next();
    auto p = ctx.path("striped.bin");
    { File f = File::open(p, write | create | truncate); }
    parallel(n, [&](unsigned t) {
        File f = File::open(p, write);
        std::vector<unsigned char> buf(stripe);
        for (size_t s = t; s < stripes; s += n) {
            pattern_fill(buf.data(), stripe, seed, s * stripe);
            f.pwrite(buf.data(), stripe, s * stripe);
        }
    });
    File f = File::open(p, read);
    CHECK_EQ(f.size(), stripe * stripes);
    std::vector<unsigned char> got(stripe), want(stripe);
    for (size_t s = 0; s < stripes; ++s) {
        f.pread(got.data(), stripe, s * stripe);
        pattern_fill(want.data(), stripe, seed, s * stripe);
        CHECK_MSG(got == want, "stripe ", s, " corrupted");
    }
}

FSB_TEST(concurrency, atomic_append, "concurrent appenders never interleave or lose records (O_APPEND atomicity)") {
    const unsigned n = worker_count();
    const int per_thread = 2000;
    const size_t rec = 512;
    auto p = ctx.path("shared.log");
    { File f = File::open(p, write | create | truncate); }
    parallel(n, [&](unsigned t) {
        File f = File::open(p, append);
        std::vector<unsigned char> r(rec);
        for (int i = 0; i < per_thread; ++i) {
            uint32_t hdr[3] = {0xFEEDF00Du, t, static_cast<uint32_t>(i)};
            std::memcpy(r.data(), hdr, sizeof(hdr));
            std::memset(r.data() + 12, static_cast<int>('a' + t % 26), rec - 20);
            uint64_t h = hash64(r.data(), rec - 8);
            std::memcpy(r.data() + rec - 8, &h, 8);
            f.write(r.data(), rec);
        }
    });
    auto data = read_file(p);
    CHECK_EQ(data.size(), static_cast<size_t>(n) * per_thread * rec);
    std::vector<int> next(n, 0);
    size_t bad = 0;
    for (size_t off = 0; off + rec <= data.size(); off += rec) {
        const auto* r = reinterpret_cast<const unsigned char*>(data.data() + off);
        uint32_t hdr[3];
        uint64_t h;
        std::memcpy(hdr, r, sizeof(hdr));
        std::memcpy(&h, r + rec - 8, 8);
        if (hdr[0] != 0xFEEDF00Du || hdr[1] >= n || h != hash64(r, rec - 8) || static_cast<int>(hdr[2]) != next[hdr[1]]) {
            ++bad;
            continue;
        }
        next[hdr[1]]++;
    }
    ctx.feature("Atomic concurrent append", bad == 0 ? "yes" : "no");
    CHECK_MSG(bad == 0, bad, " torn/out-of-order records");
    for (unsigned t = 0; t < n; ++t) CHECK_EQ(next[t], per_thread);
}

FSB_TEST(concurrency, exclusive_create_race, "racing exclusive creates: exactly one thread wins each round") {
    const unsigned n = worker_count();
    std::barrier sync(static_cast<std::ptrdiff_t>(n));
    std::vector<std::atomic<int>> wins(100);
    parallel(n, [&](unsigned) {
        for (int round = 0; round < 100; ++round) {
            sync.arrive_and_wait();
            std::error_code ec;
            File f = File::open(ctx.path(cat("race_", round)), write | create | exclusive, ec);
            if (!ec) wins[round]++;
            else if (plat::classify(ec) != ErrKind::Exists) throw std::system_error(ec);
        }
    });
    for (int r = 0; r < 100; ++r) CHECK_MSG(wins[r] == 1, "round ", r, ": ", wins[r].load(), " winners");
}

FSB_TEST(concurrency, mkdir_race, "racing mkdir of the same directory: exactly one thread creates it") {
    const unsigned n = worker_count();
    std::barrier sync(static_cast<std::ptrdiff_t>(n));
    std::vector<std::atomic<int>> wins(100);
    parallel(n, [&](unsigned) {
        for (int round = 0; round < 100; ++round) {
            sync.arrive_and_wait();
            if (fs::create_directory(ctx.path(cat("dir_", round)))) wins[round]++;
        }
    });
    for (int r = 0; r < 100; ++r) CHECK_MSG(wins[r] == 1, "round ", r, ": ", wins[r].load(), " creators");
}

FSB_TEST(concurrency, rename_race, "racing renames of one source: exactly one succeeds, nothing is lost") {
    const unsigned n = worker_count();
    std::barrier sync(static_cast<std::ptrdiff_t>(n));
    std::vector<std::atomic<int>> wins(100);
    for (int round = 0; round < 100; ++round) write_file(ctx.path(cat("src_", round)), cat("payload", round));
    parallel(n, [&](unsigned t) {
        for (int round = 0; round < 100; ++round) {
            sync.arrive_and_wait();
            std::error_code ec;
            plat::rename_noreplace(ctx.path(cat("src_", round)), ctx.path(cat("dst_", round, "_", t)), ec);
            if (plat::is_unsupported(ec))
                ec = capture_error([&] { plat::rename_replace(ctx.path(cat("src_", round)), ctx.path(cat("dst_", round, "_", t))); });
            if (!ec) wins[round]++;
        }
    });
    for (int r = 0; r < 100; ++r) {
        CHECK_MSG(wins[r] == 1, "round ", r, ": ", wins[r].load(), " successful renames");
        CHECK(!fs::exists(ctx.path(cat("src_", r))));
    }
    CHECK_EQ(list_names(ctx.root).size(), 100u);
}

FSB_TEST(concurrency, readers_during_writes, "readers racing a writer only see whole 4 KiB blocks (torn-read check)") {
    auto p = ctx.path("rw.bin");
    const size_t blk = 4096, nblk = 256;
    {
        File f = File::open(p, write | create | truncate);
        std::string z(blk * nblk, 'A');
        f.pwrite(z.data(), z.size(), 0);
    }
    std::atomic<bool> stop{false};
    std::atomic<long> torn{0}, reads{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r)
        readers.emplace_back([&, r] {
            File f = File::open(p, read);
            Rng rng(r + 1);
            std::string b(blk, '\0');
            while (!stop) {
                f.pread(b.data(), blk, rng.below(nblk) * blk);
                reads++;
                if (b.find_first_not_of(b[0]) != std::string::npos) torn++;
            }
        });
    {
        File f = File::open(p, write);
        std::string b(blk, '\0');
        Timer t;
        for (int v = 0; t.elapsed_s() < 1.5; ++v) {
            std::memset(b.data(), 'A' + v % 26, blk);
            f.pwrite(b.data(), blk, ctx.rng.below(nblk) * blk);
        }
    }
    stop = true;
    for (auto& t : readers) t.join();
    ctx.note(cat(reads.load(), " reads, ", torn.load(), " torn"));
    ctx.feature("Torn 4K reads under concurrent writes", torn == 0 ? "none observed" : cat(torn.load(), " observed"));
}

}  // namespace fsb
