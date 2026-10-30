// File-operation benchmarks: copy (several methods), tree copy, move, delete, atomic save.
#include <mutex>

#include "bench_util.hpp"

namespace fsb {
using namespace bu;

namespace {

void emit(BenchContext& ctx, BenchResult r, const std::string& name, const std::string& params) {
    r.category = "ops";
    r.name = name;
    r.params = params;
    ctx.report(std::move(r));
}

// Tree of `files` files with a realistic size mix (mostly small, a few large).
uint64_t build_tree(const fs::path& root, Rng& rng, size_t files) {
    std::vector<fs::path> dirs;
    for (size_t i = 0; i < std::max<size_t>(files / 50, 1); ++i) {
        fs::path d = root / cat("dir", i % 10) / cat("sub", i);
        fs::create_directories(d);
        dirs.push_back(d);
    }
    AlignedBuffer buf(1 << 20);
    rng.fill(buf.data(), buf.size());
    uint64_t total = 0;
    for (size_t i = 0; i < files; ++i) {
        uint64_t roll = rng.below(100);
        size_t sz = roll < 70 ? 1 + rng.below(8192) : roll < 95 ? 8192 + rng.below(57344) : 65536 + rng.below(983040);
        File f = File::open(dirs[i % dirs.size()] / cat("f", i, ".bin"), write | create | truncate);
        f.pwrite(buf.data(), sz, 0);
        total += sz;
    }
    return total;
}

std::vector<fs::path> list_files(const fs::path& root) {
    std::vector<fs::path> out;
    for (auto& e : fs::recursive_directory_iterator(root))
        if (e.is_regular_file()) out.push_back(fs::relative(e.path(), root));
    return out;
}

void sync_file(const fs::path& p) { File::open(p, read | write).sync(); }

}  // namespace

FSB_BENCH(ops, copy_file, "copy one large file with each method (cold source, destination fsync included)") {
    fs::path src = ctx.shared_data_file(ctx.cfg.file_size);
    const uint64_t size = fs::file_size(src);
    struct Method {
        std::string name;
        std::function<void(const fs::path&, const fs::path&)> fn;
    };
    std::vector<Method> methods = {
        {"read/write 1M", [](auto& s, auto& d) { plat::copy_readwrite(s, d, 1 << 20); }},
        {"read/write 64K", [](auto& s, auto& d) { plat::copy_readwrite(s, d, 64 << 10); }},
        {"kernel copy", [](auto& s, auto& d) { plat::copy_kernel(s, d); }},
        {"std::fs::copy_file", [](auto& s, auto& d) { fs::copy_file(s, d, fs::copy_options::overwrite_existing); }},
        {"mmap", [](auto& s, auto& d) { plat::copy_mmap(s, d); }},
    };
    if (ctx.direct_io_supported())
        methods.push_back({"read/write 4M direct", [](auto& s, auto& d) { plat::copy_readwrite(s, d, 4 << 20, true); }});
    std::error_code ec, ignore;
    plat::copy_reflink(src, ctx.path("probe.clone"), ec);
    bool reflink = !ec;
    fs::remove(ctx.path("probe.clone"), ignore);
    if (reflink)
        methods.push_back({"reflink (CoW clone)", [](auto& s, auto& d) {
                               std::error_code e;
                               plat::copy_reflink(s, d, e);
                               if (e) throw std::system_error(e);
                           }});
    for (auto& m : methods) {
        fs::path dst = ctx.path("copy.dst");
        plat::drop_file_cache(src);
        Timer t;
        m.fn(src, dst);
        sync_file(dst);
        BenchResult r;
        r.seconds = t.elapsed_s();
        r.bytes = size;
        r.ops = 1;
        emit(ctx, r, "copy_file", cat(size_name(size), " ", m.name));
        fs::remove(dst);
    }
}

FSB_BENCH(ops, copy_tree, "copy a tree of mixed-size files: std::filesystem (1 thread) vs N threads") {
    fs::path src = ctx.path("src");
    const size_t n = ctx.cfg.tree_files;
    const uint64_t bytes = build_tree(src, ctx.rng, n);
    plat::sync_filesystem(src);
    auto rels = list_files(src);
    const std::string base = cat("files=", n, " ", size_name(bytes / (1 << 20) * (1 << 20)));
    {
        Timer t;
        fs::copy(src, ctx.path("dst_std"), fs::copy_options::recursive);
        bool synced = plat::sync_filesystem(ctx.path("dst_std"));
        BenchResult r;
        r.seconds = t.elapsed_s();
        r.bytes = bytes;
        r.ops = n;
        r.note = synced ? "incl. syncfs" : "no fs-wide sync available";
        emit(ctx, r, "copy_tree", base + " std::fs::copy");
        fs::remove_all(ctx.path("dst_std"));
    }
    for (unsigned th : ctx.cfg.threads) {
        if (th < 2) continue;
        fs::path dst = ctx.path(cat("dst_", th));
        Timer t;
        for (auto& e : fs::recursive_directory_iterator(src))
            if (e.is_directory()) fs::create_directories(dst / fs::relative(e.path(), src));
        std::atomic<size_t> next{0};
        run_threads(th, [&](unsigned) {
            for (size_t i; (i = next++) < rels.size();) plat::copy_kernel(src / rels[i], dst / rels[i]);
        });
        bool synced = plat::sync_filesystem(dst);
        BenchResult r;
        r.seconds = t.elapsed_s();
        r.bytes = bytes;
        r.ops = n;
        r.note = synced ? "incl. syncfs" : "no fs-wide sync available";
        emit(ctx, r, "copy_tree", cat(base, " kernel copy T=", th));
        fs::remove_all(dst);
    }
}

FSB_BENCH(ops, delete_tree, "remove_all of a tree of mixed-size files") {
    const size_t n = ctx.cfg.tree_files;
    fs::path root = ctx.path("victim");
    build_tree(root, ctx.rng, n);
    plat::sync_filesystem(root);
    Timer t;
    auto removed = fs::remove_all(root);
    BenchResult r;
    r.seconds = t.elapsed_s();
    r.ops = static_cast<uint64_t>(removed);
    emit(ctx, r, "delete_tree", cat("files=", n));
}

FSB_BENCH(ops, delete_large, "time to delete and to truncate one large, fully allocated file") {
    for (const char* op : {"unlink", "truncate"}) {
        fs::path p = ctx.path("large.bin");
        fill_file(p, ctx.cfg.file_size, 99);
        Timer t;
        if (op[0] == 'u') {
            fs::remove(p);
        } else {
            File f = File::open(p, write);
            f.truncate(0);
            f.sync();
        }
        BenchResult r;
        r.seconds = t.elapsed_s();
        r.ops = 1;
        emit(ctx, r, "delete_large", cat(size_name(ctx.cfg.file_size), " ", op));
        std::error_code ec;
        fs::remove(p, ec);
    }
}

FSB_BENCH(ops, atomic_save, "crash-safe save pattern: write temp + fsync + rename + dir fsync") {
    const size_t n = std::min<size_t>(ctx.cfg.meta_files / 10, 1000);
    AlignedBuffer buf(16 << 10);
    ctx.rng.fill(buf.data(), buf.size());
    fs::path target = ctx.path("document.json"), tmp = ctx.path("document.json.tmp");
    std::error_code ec;
    File dir = File::open_dir(ctx.root, ec);
    bool dir_sync = !ec && !capture_error([&] { dir.sync(); });
    LatencyRecorder rec;
    Timer t;
    for (size_t i = 0; i < n; ++i) {
        uint64_t t0 = now_ns();
        {
            File f = File::open(tmp, write | create | truncate);
            f.pwrite(buf.data(), buf.size(), 0);
            f.sync();
        }
        plat::rename_replace(tmp, target);
        if (dir_sync) dir.sync();
        rec.add(now_ns() - t0);
    }
    BenchResult r;
    r.seconds = t.elapsed_s();
    r.ops = n;
    r.has_latency = true;
    r.lat = rec.summarize();
    if (!dir_sync) r.note = "directory fsync unavailable";
    emit(ctx, r, "atomic_save", cat("16K N=", n));
}

FSB_BENCH(ops, cross_device, "copy and move a large file and a tree to --alt-target (copy + delete)") {
    if (!ctx.alt_root) ctx.skip("needs --alt-target");
    if (plat::stat_path(ctx.root).device == plat::stat_path(*ctx.alt_root).device) ctx.skip("alt target is on the same filesystem");
    const uint64_t size = std::min<uint64_t>(ctx.cfg.file_size, 1ull << 30);
    fs::path src = ctx.path("x.bin");
    fill_file(src, size, 5);
    const std::string alt = "-> " + plat::fs_info(*ctx.alt_root).type;
    {
        plat::drop_file_cache(src);
        Timer t;
        plat::copy_kernel(src, *ctx.alt_root / "x.copy");
        sync_file(*ctx.alt_root / "x.copy");
        BenchResult r;
        r.seconds = t.elapsed_s();
        r.bytes = size;
        r.ops = 1;
        emit(ctx, r, "cross_device", cat("copy ", size_name(size), " ", alt));
    }
    {
        plat::drop_file_cache(src);
        Timer t;
        plat::move_path(src, *ctx.alt_root / "x.moved");
        sync_file(*ctx.alt_root / "x.moved");
        BenchResult r;
        r.seconds = t.elapsed_s();
        r.bytes = size;
        r.ops = 1;
        emit(ctx, r, "cross_device", cat("move ", size_name(size), " ", alt));
    }
    {
        const size_t n = std::min<size_t>(ctx.cfg.tree_files, 5000);
        fs::path tree = ctx.path("tree");
        uint64_t bytes = build_tree(tree, ctx.rng, n);
        plat::sync_filesystem(tree);
        Timer t;
        plat::move_path(tree, *ctx.alt_root / "tree");
        plat::sync_filesystem(*ctx.alt_root);
        BenchResult r;
        r.seconds = t.elapsed_s();
        r.bytes = bytes;
        r.ops = n;
        emit(ctx, r, "cross_device", cat("move tree files=", n, " ", alt));
    }
}

}  // namespace fsb
