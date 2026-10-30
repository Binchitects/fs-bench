// Metadata benchmarks: create/stat/open/readdir/rename/delete rates, small files, links, parallelism.
#include <cstring>

#include "bench_util.hpp"

namespace fsb {
using namespace bu;

namespace {

template <typename F>
BenchResult time_ops(size_t n, F op) {
    LatencyRecorder rec;
    rec.reserve(n);
    Timer t;
    for (size_t i = 0; i < n; ++i) {
        uint64_t t0 = now_ns();
        op(i);
        rec.add(now_ns() - t0);
    }
    BenchResult r;
    r.seconds = t.elapsed_s();
    r.ops = n;
    r.has_latency = true;
    r.lat = rec.summarize();
    return r;
}

void emit(BenchContext& ctx, BenchResult r, const std::string& name, const std::string& params) {
    r.category = "meta";
    r.name = name;
    r.params = params;
    ctx.report(std::move(r));
}

std::string fname(size_t i) { return cat("file_", i, ".dat"); }

}  // namespace

FSB_BENCH(meta, file_ops, "N empty files: create, stat, open/close, readdir, utimes, rename, delete") {
    const size_t n = ctx.cfg.meta_files;
    const std::string pr = cat("N=", n);
    fs::path d = ctx.path("files"), d2 = ctx.path("moved");
    fs::create_directories(d);
    fs::create_directories(d2);
    emit(ctx, time_ops(n, [&](size_t i) { File::open(d / fname(i), write | create | exclusive); }), "create", pr);
    emit(ctx, time_ops(n, [&](size_t i) { plat::stat_path(d / fname(i)); }), "stat", pr);
    emit(ctx, time_ops(n, [&](size_t i) { File::open(d / fname(i), read); }), "open_close", pr);
    {
        // Enumerate the directory repeatedly; one op = one entry returned.
        size_t entries = 0;
        LatencyRecorder rec;
        Timer t;
        do {
            uint64_t t0 = now_ns();
            for (auto& e : fs::directory_iterator(d)) {
                (void)e;
                ++entries;
            }
            rec.add(now_ns() - t0);
        } while (t.elapsed_s() < 1.0 && rec.size() < 50);
        BenchResult r;
        r.seconds = t.elapsed_s();
        r.ops = entries;
        r.note = cat("full listing avg ", fmt_duration_ns(rec.summarize().mean_ns));
        emit(ctx, r, "readdir", pr);
    }
    const int64_t ts = 1600000000LL * 1000000000LL;
    emit(ctx, time_ops(n, [&](size_t i) { plat::set_file_times(d / fname(i), ts, ts); }), "set_times", pr);
    emit(ctx, time_ops(n, [&](size_t i) { plat::rename_replace(d / fname(i), d / cat("r_", i)); }), "rename", pr);
    emit(ctx, time_ops(n, [&](size_t i) { plat::rename_replace(d / cat("r_", i), d2 / cat("r_", i)); }),
         "rename_cross_dir", pr);
    emit(ctx, time_ops(n, [&](size_t i) { fs::remove(d2 / cat("r_", i)); }), "delete", pr);
}

FSB_BENCH(meta, small_files, "4 KiB files: create+write, read (warm), create+write+fsync, delete") {
    const size_t n = ctx.cfg.meta_files;
    fs::path d = ctx.path("small");
    fs::create_directories(d);
    AlignedBuffer buf(4096);
    ctx.rng.fill(buf.data(), buf.size());
    auto r = time_ops(n, [&](size_t i) {
        File f = File::open(d / fname(i), write | create | truncate);
        f.pwrite(buf.data(), 4096, 0);
    });
    r.bytes = n * 4096;
    emit(ctx, r, "create_write_4k", cat("N=", n));
    r = time_ops(n, [&](size_t i) {
        File f = File::open(d / fname(i), read);
        f.pread(buf.data(), 4096, 0);
    });
    r.bytes = n * 4096;
    emit(ctx, r, "read_4k", cat("N=", n, " warm"));
    const size_t ns = std::min<size_t>(n, 2000);
    r = time_ops(ns, [&](size_t i) {
        File f = File::open(d / cat("sync_", i), write | create | truncate);
        f.pwrite(buf.data(), 4096, 0);
        f.sync();
    });
    r.bytes = ns * 4096;
    emit(ctx, r, "create_write_4k_fsync", cat("N=", ns));
    emit(ctx, time_ops(n, [&](size_t i) { fs::remove(d / fname(i)); }), "delete_4k", cat("N=", n));
}

FSB_BENCH(meta, mkdir_rmdir, "create and remove N directories") {
    const size_t n = ctx.cfg.meta_files;
    fs::path d = ctx.path("dirs");
    fs::create_directories(d);
    emit(ctx, time_ops(n, [&](size_t i) { fs::create_directory(d / cat("d", i)); }), "mkdir", cat("N=", n));
    emit(ctx, time_ops(n, [&](size_t i) { fs::remove(d / cat("d", i)); }), "rmdir", cat("N=", n));
}

FSB_BENCH(meta, links, "hard link and symbolic link creation rates") {
    const size_t n = ctx.cfg.meta_files;
    fs::path d = ctx.path("links");
    fs::create_directories(d);
    File::open(d / "target", write | create);
    std::error_code ec;
    fs::create_hard_link(d / "target", d / "probe_h", ec);
    if (!ec)
        emit(ctx, time_ops(n, [&](size_t i) { fs::create_hard_link(d / "target", d / cat("h", i)); }), "hardlink",
             cat("N=", n));
    else
        ctx.feature("Hard links (bench)", "unsupported: " + ec.message());
    fs::create_symlink("target", d / "probe_s", ec);
    if (!ec)
        emit(ctx, time_ops(n, [&](size_t i) { fs::create_symlink("target", d / cat("s", i)); }), "symlink",
             cat("N=", n));
    else
        ctx.feature("Symlinks (bench)", "unavailable: " + ec.message());
}

FSB_BENCH(meta, parallel_create, "concurrent file creation: shared directory vs. one directory per thread") {
    const size_t n = ctx.cfg.meta_files;
    for (unsigned t : ctx.cfg.threads) {
        if (t < 2) continue;
        for (bool shared : {true, false}) {
            fs::path base = ctx.path(cat("pc_", t, shared ? "_shared" : "_split"));
            for (unsigned i = 0; i < t; ++i) fs::create_directories(shared ? base : base / cat("t", i));
            const size_t per = n / t;
            std::vector<LatencyRecorder> recs(t);
            uint64_t ns = run_threads(t, [&](unsigned th) {
                fs::path dir = shared ? base : base / cat("t", th);
                for (size_t i = 0; i < per; ++i) {
                    uint64_t t0 = now_ns();
                    File::open(dir / cat("f_", th, "_", i), write | create | exclusive);
                    recs[th].add(now_ns() - t0);
                }
            });
            for (unsigned i = 1; i < t; ++i) recs[0].merge(recs[i]);
            BenchResult r;
            r.seconds = static_cast<double>(ns) / 1e9;
            r.ops = per * t;
            r.has_latency = true;
            r.lat = recs[0].summarize();
            emit(ctx, r, "parallel_create", cat("N=", per * t, " T=", t, shared ? " same-dir" : " dir-per-thread"));
            fs::remove_all(base);
        }
    }
}

FSB_BENCH(meta, tree_walk, "recursive traversal + stat of a deep directory tree") {
    // 3 levels x fan-out 10 directories, files spread over the leaves.
    const size_t n = ctx.cfg.meta_files;
    fs::path root = ctx.path("tree");
    std::vector<fs::path> leaves;
    for (int a = 0; a < 10; ++a)
        for (int b = 0; b < 10; ++b)
            for (int c = 0; c < 10; ++c) leaves.push_back(root / cat("a", a) / cat("b", b) / cat("c", c));
    auto rb = time_ops(leaves.size(), [&](size_t i) { fs::create_directories(leaves[i]); });
    emit(ctx, rb, "tree_mkdirs", cat("dirs=", leaves.size()));
    for (size_t i = 0; i < n; ++i) File::open(leaves[i % leaves.size()] / fname(i), write | create);
    size_t entries = 0;
    Timer t;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        plat::stat_path(it->path(), false);
        ++entries;
    }
    BenchResult r;
    r.seconds = t.elapsed_s();
    r.ops = entries;
    r.note = "warm cache";
    emit(ctx, r, "tree_walk_stat", cat("entries=", entries));
}

}  // namespace fsb
