// Rename, move, copy and delete semantics.
#include <atomic>
#include <map>
#include <thread>

#include "test_util.hpp"

namespace fsb {
using namespace tu;

namespace {

std::map<std::string, uint64_t> snapshot(const fs::path& root) {
    std::map<std::string, uint64_t> m;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        auto rel = path_str(fs::relative(it->path(), root));
        m[rel] = it->is_regular_file() ? hash_file(it->path()) : 0;
    }
    return m;
}

}  // namespace

// ---------------------------------------------------------------- rename

FSB_TEST(ops, rename_file, "rename within a directory and across directories keeps content and identity") {
    auto d = random_bytes(ctx.rng, 100000);
    write_file(ctx.path("a"), d);
    auto ino = plat::stat_path(ctx.path("a")).inode;
    plat::rename_replace(ctx.path("a"), ctx.path("b"));
    CHECK(!fs::exists(ctx.path("a")));
    CHECK(read_file(ctx.path("b")) == d);
    fs::create_directories(ctx.path("x/y"));
    plat::rename_replace(ctx.path("b"), ctx.path("x/y/c"));
    CHECK(read_file(ctx.path("x/y/c")) == d);
    CHECK_EQ(plat::stat_path(ctx.path("x/y/c")).inode, ino);
}

FSB_TEST(ops, rename_overwrite, "rename onto an existing file replaces it") {
    write_file(ctx.path("src"), "new");
    write_file(ctx.path("dst"), "old-content");
    auto src_ino = plat::stat_path(ctx.path("src")).inode;
    plat::rename_replace(ctx.path("src"), ctx.path("dst"));
    CHECK(!fs::exists(ctx.path("src")));
    CHECK(read_file(ctx.path("dst")) == "new");
    CHECK_EQ(plat::stat_path(ctx.path("dst")).inode, src_ino);
    CHECK_EQ(list_names(ctx.root).size(), 1u);
}

FSB_TEST(ops, rename_atomic_replace, "write-temp-then-rename never exposes a missing or partial target to readers") {
    auto target = ctx.path("config.json");
    const size_t sz = 64 << 10;
    auto make = [&](int v) {
        std::string s(sz, static_cast<char>('A' + v % 26));
        return s;
    };
    write_file(target, make(0));
    std::atomic<bool> stop{false};
    std::atomic<long> missing{0}, torn{0}, reads{0}, other_err{0};
    std::thread reader([&] {
        while (!stop) {
            std::error_code ec;
            File f = File::open(target, read, ec);
            if (ec) {
                (plat::classify(ec) == ErrKind::NotFound ? missing : other_err)++;
                continue;
            }
            std::string s(sz, '\0');
            size_t n = 0;
            auto e = capture_error([&] { n = f.pread(s.data(), sz, 0); });
            if (e) {
                other_err++;
                continue;
            }
            reads++;
            if (n != sz || s.find_first_not_of(s[0]) != std::string::npos) torn++;
        }
    });
    int replace_errors = 0;
    std::string first_err;
    for (int v = 1; v <= 300; ++v) {
        auto tmp = ctx.path(cat("config.json.tmp", v));
        write_file(tmp, make(v));
        auto ec = capture_error([&] { plat::rename_replace(tmp, target); });
        if (ec) {
            if (!replace_errors++) first_err = ec.message();
            std::error_code ignore;
            fs::remove(tmp, ignore);
        }
    }
    stop = true;
    reader.join();
    ctx.note(cat(reads.load(), " reads, ", missing.load(), " missing, ", torn.load(), " torn, ", other_err.load(),
                 " open/read errors, ", replace_errors, " failed renames"));
    ctx.feature("Atomic rename-replace", missing == 0 && torn == 0 && replace_errors == 0 ? "yes" : "no");
    CHECK_MSG(missing == 0, "target was briefly missing ", missing.load(), " times");
    CHECK_MSG(torn == 0, "reader saw torn content ", torn.load(), " times");
    CHECK_MSG(replace_errors == 0, replace_errors, " renames failed while a reader had the file open: ", first_err);
}

FSB_TEST(ops, rename_noreplace, "no-replace rename refuses to clobber an existing target") {
    write_file(ctx.path("a"), "A");
    write_file(ctx.path("b"), "B");
    std::error_code ec;
    plat::rename_noreplace(ctx.path("a"), ctx.path("b"), ec);
    if (plat::is_unsupported(ec) || (ec && plat::classify(ec) == ErrKind::InvalidArg)) {
        ctx.feature("Rename no-replace", "no");
        ctx.unsupported("RENAME_NOREPLACE not supported: " + ec.message());
    }
    CHECK_EQ(plat::classify(ec), ErrKind::Exists);
    CHECK(read_file(ctx.path("a")) == "A");
    CHECK(read_file(ctx.path("b")) == "B");
    plat::rename_noreplace(ctx.path("a"), ctx.path("c"), ec);
    CHECK(!ec);
    CHECK(read_file(ctx.path("c")) == "A");
    ctx.feature("Rename no-replace", "yes");
}

FSB_TEST(ops, rename_exchange, "atomic exchange of two names (RENAME_EXCHANGE)") {
    write_file(ctx.path("a"), "A");
    fs::create_directory(ctx.path("b"));
    std::error_code ec;
    plat::rename_exchange(ctx.path("a"), ctx.path("b"), ec);
    if (ec) {
        ctx.feature("Rename exchange", "no");
        ctx.unsupported("atomic exchange not supported: " + ec.message());
    }
    CHECK(fs::is_directory(ctx.path("a")));
    CHECK(read_file(ctx.path("b")) == "A");
    ctx.feature("Rename exchange", "yes");
}

FSB_TEST(ops, rename_directory, "rename a populated directory; contents follow") {
    make_tree(ctx.path("tree"), ctx.rng, 10, 100);
    auto before = snapshot(ctx.path("tree"));
    fs::create_directory(ctx.path("parent"));
    plat::rename_replace(ctx.path("tree"), ctx.path("parent/moved"));
    CHECK(!fs::exists(ctx.path("tree")));
    CHECK(snapshot(ctx.path("parent/moved")) == before);
}

FSB_TEST(ops, rename_dir_edge_cases, "invalid directory renames fail and leave everything intact") {
    fs::create_directories(ctx.path("d/sub"));
    write_file(ctx.path("d/sub/f"), "f");
    // Into its own subdirectory.
    CHECK_THROWS(plat::rename_replace(ctx.path("d"), ctx.path("d/sub/inner")));
    CHECK(read_file(ctx.path("d/sub/f")) == "f");
    // Directory over a non-empty directory.
    fs::create_directory(ctx.path("full"));
    touch(ctx.path("full/x"));
    CHECK_THROWS(plat::rename_replace(ctx.path("d"), ctx.path("full")));
    CHECK(fs::exists(ctx.path("full/x")));
    // File over a directory and directory over a file.
    write_file(ctx.path("file"), "file");
    CHECK_THROWS(plat::rename_replace(ctx.path("file"), ctx.path("full")));
    CHECK_THROWS(plat::rename_replace(ctx.path("full"), ctx.path("file")));
    CHECK(read_file(ctx.path("file")) == "file");
    // Directory over an empty directory: allowed by POSIX, refused by Win32.
    fs::create_directory(ctx.path("empty"));
    auto ec = capture_error([&] { plat::rename_replace(ctx.path("d"), ctx.path("empty")); });
    ctx.feature("Rename dir over empty dir", ec ? "refused" : "replaces");
    // Missing source.
    CHECK_ERR(plat::rename_replace(ctx.path("nope"), ctx.path("nope2")), NotFound);
}

FSB_TEST(ops, rename_open_file, "renaming a file that is open keeps the handle valid") {
    File f = File::open(ctx.path("open.bin"), kCreateTruncRW);
    f.pwrite("before", 6, 0);
    plat::rename_replace(ctx.path("open.bin"), ctx.path("renamed.bin"));
    f.pwrite("-after", 6, 6);
    f.close();
    CHECK(read_file(ctx.path("renamed.bin")) == "before-after");
}

FSB_TEST(ops, rename_same_path, "renaming a file onto itself is a harmless no-op") {
    write_file(ctx.path("same"), "same");
    auto ec = capture_error([&] { plat::rename_replace(ctx.path("same"), ctx.path("same")); });
    CHECK(read_file(ctx.path("same")) == "same");
    if (ec) ctx.note("rename(a, a) returned an error: " + ec.message());
}

// ---------------------------------------------------------------- move

FSB_TEST(ops, move_tree_same_fs, "moving a tree within a filesystem is a cheap rename that preserves everything") {
    make_tree(ctx.path("src"), ctx.rng, 20, 300);
    auto before = snapshot(ctx.path("src"));
    auto ino = plat::stat_path(ctx.path("src")).inode;
    Timer t;
    plat::move_path(ctx.path("src"), ctx.path("dst"));
    ctx.note(cat("moved 300 files in ", fmt_duration_ns(static_cast<double>(t.elapsed_ns()))));
    CHECK(!fs::exists(ctx.path("src")));
    CHECK(snapshot(ctx.path("dst")) == before);
    CHECK_EQ(plat::stat_path(ctx.path("dst")).inode, ino);
}

FSB_TEST(ops, move_cross_device, "moving files and trees to another filesystem (copy + delete fallback)") {
    if (!ctx.alt_root) ctx.skip("needs --alt-target on a different filesystem");
    if (plat::stat_path(ctx.root).device == plat::stat_path(*ctx.alt_root).device)
        ctx.skip("--alt-target is on the same filesystem");
    auto d = random_bytes(ctx.rng, 3 << 20);
    write_file(ctx.path("file.bin"), d);
    // A plain rename must refuse with a cross-device error.
    CHECK_ERR(plat::rename_replace(ctx.path("file.bin"), *ctx.alt_root / "file.bin"), CrossDevice);
    plat::move_path(ctx.path("file.bin"), *ctx.alt_root / "file.bin");
    CHECK(!fs::exists(ctx.path("file.bin")));
    CHECK(read_file(*ctx.alt_root / "file.bin") == d);

    make_tree(ctx.path("tree"), ctx.rng, 10, 200);
    auto before = snapshot(ctx.path("tree"));
    plat::move_path(ctx.path("tree"), *ctx.alt_root / "tree");
    CHECK(!fs::exists(ctx.path("tree")));
    CHECK(snapshot(*ctx.alt_root / "tree") == before);
    // And back again.
    plat::move_path(*ctx.alt_root / "tree", ctx.path("back"));
    CHECK(snapshot(ctx.path("back")) == before);
}

// ---------------------------------------------------------------- copy

FSB_TEST(ops, copy_methods, "every copy method (read/write, kernel, mmap, std::filesystem) copies exactly") {
    struct Method {
        const char* name;
        std::function<void(const fs::path&, const fs::path&)> fn;
    };
    std::vector<Method> methods = {
        {"readwrite", [](auto& s, auto& d) { plat::copy_readwrite(s, d, 1 << 20); }},
        {"readwrite-direct",
         [&](auto& s, auto& d) {
             if (capture_error([&] { plat::copy_readwrite(s, d, 1 << 20, true); }))
                 plat::copy_readwrite(s, d, 1 << 20);  // fs without direct I/O
         }},
        {"kernel", [](auto& s, auto& d) { plat::copy_kernel(s, d); }},
        {"mmap", [](auto& s, auto& d) { plat::copy_mmap(s, d); }},
        {"std::filesystem",
         [](auto& s, auto& d) { fs::copy_file(s, d, fs::copy_options::overwrite_existing); }},
    };
    for (size_t sz : {size_t(0), size_t(1), size_t(4095), size_t(1 << 20) + 7, size_t(24 << 20) + 4096 + 3}) {
        auto src = ctx.path(cat("src_", sz));
        write_file(src, random_bytes(ctx.rng, sz));
        for (auto& m : methods) {
            auto dst = ctx.path(cat("dst_", m.name[0], "_", sz));
            m.fn(src, dst);
            CHECK_MSG(files_equal(src, dst), m.name, " produced a different file for size ", sz);
            fs::remove(dst);
        }
    }
}

FSB_TEST(ops, copy_reflink, "copy-on-write clone (FICLONE / block cloning) is exact and independent") {
    auto src = ctx.path("src.bin"), dst = ctx.path("clone.bin");
    auto d = random_bytes(ctx.rng, 8 << 20);
    write_file(src, d);
    std::error_code ec;
    plat::copy_reflink(src, dst, ec);
    if (ec) {
        ctx.feature("Reflink / block clone", "no");
        ctx.unsupported("reflink not supported: " + ec.message());
    }
    CHECK(read_file(dst) == d);
    {
        File f = File::open(dst, write);
        f.pwrite("MODIFIED", 8, 4 << 20);
    }
    CHECK_MSG(read_file(src) == d, "writing the clone modified the source");
    ctx.feature("Reflink / block clone", "yes");
}

FSB_TEST(ops, copy_overwrite_options, "copy_file honours none / skip_existing / overwrite_existing / update_existing") {
    write_file(ctx.path("src"), "source");
    write_file(ctx.path("dst"), "dest");
    CHECK_ERR(fs::copy_file(ctx.path("src"), ctx.path("dst")), Exists);
    CHECK(read_file(ctx.path("dst")) == "dest");
    CHECK(!fs::copy_file(ctx.path("src"), ctx.path("dst"), fs::copy_options::skip_existing));
    CHECK(read_file(ctx.path("dst")) == "dest");
    CHECK(fs::copy_file(ctx.path("src"), ctx.path("dst"), fs::copy_options::overwrite_existing));
    CHECK(read_file(ctx.path("dst")) == "source");
    // update_existing: only copies when the source is newer.
    plat::set_file_times(ctx.path("src"), 1000000000LL * 1000000000LL, 1000000000LL * 1000000000LL);
    write_file(ctx.path("dst"), "newer-dest");
    CHECK(!fs::copy_file(ctx.path("src"), ctx.path("dst"), fs::copy_options::update_existing));
    CHECK(read_file(ctx.path("dst")) == "newer-dest");
}

FSB_TEST(ops, copy_onto_self, "copying a file onto itself fails without destroying the data") {
    auto d = random_bytes(ctx.rng, 100000);
    write_file(ctx.path("self"), d);
    auto ec = capture_error([&] { fs::copy_file(ctx.path("self"), ctx.path("self"), fs::copy_options::overwrite_existing); });
    CHECK_MSG(read_file(ctx.path("self")) == d, "copy onto itself corrupted the file");
    CHECK_MSG(ec, "copy onto itself reported success");
}

FSB_TEST(ops, copy_metadata, "which metadata (mode, mtime, xattrs, sparseness) each copy method preserves") {
    auto src = ctx.path("meta_src");
    {
        File f = File::open(src, kCreateTruncRW);
        std::error_code ec;
        f.set_sparse(ec);
        f.pwrite("head", 4, 0);
        f.pwrite("tail", 4, 32ull << 20);  // 32 MiB hole in between
    }
    const int64_t old = 1300000000LL * 1000000000LL;
    plat::set_file_times(src, old, old);
#ifndef _WIN32
    fs::permissions(src, static_cast<fs::perms>(0640), fs::perm_options::replace);
#endif
    std::error_code xec;
    plat::xattr_set(src, "fsbench.tag", "v", xec);
    bool have_xattr = !xec;
    const uint64_t src_alloc = plat::stat_path(src).allocated;

    struct M {
        const char* name;
        std::function<void(const fs::path&, const fs::path&)> fn;
    };
    for (auto& m : std::vector<M>{
             {"std::filesystem", [](auto& s, auto& d) { fs::copy_file(s, d); }},
             {"kernel", [](auto& s, auto& d) { plat::copy_kernel(s, d); }},
             {"readwrite", [](auto& s, auto& d) { plat::copy_readwrite(s, d, 1 << 20); }},
         }) {
        auto dst = ctx.path(cat("copy_", m.name[0]));
        m.fn(src, dst);
        CHECK(files_equal(src, dst));
        auto st = plat::stat_path(dst);
        std::string kept;
        if (st.mtime_ns == old) kept += " mtime";
#ifndef _WIN32
        if (st.mode == 0640) kept += " mode";
#endif
        if (have_xattr) {
            std::error_code e;
            if (plat::xattr_get(dst, "fsbench.tag", e) == "v" && !e) kept += " xattr";
        }
        if (src_alloc < (16ull << 20) && st.allocated < (16ull << 20)) kept += " holes";
        ctx.feature(std::string("Copy (") + m.name + ") keeps", kept.empty() ? "content only" : kept.substr(1));
    }
}

FSB_TEST(ops, copy_tree, "recursive copy of a directory tree reproduces it exactly") {
    make_tree(ctx.path("src"), ctx.rng, 15, 250, 65536);
    fs::create_directories(ctx.path("src/empty/dir"));
    fs::copy(ctx.path("src"), ctx.path("dst"), fs::copy_options::recursive);
    std::string why;
    CHECK_MSG(trees_equal(ctx.path("src"), ctx.path("dst"), &why), why);
}

FSB_TEST(ops, copy_large_integrity, "256 MiB copied with the kernel path verifies by hash") {
    auto src = ctx.path("big.src");
    const uint64_t total = 256ull << 20;
    {
        File f = File::open(src, write | create | truncate);
        std::vector<unsigned char> buf(4 << 20);
        for (uint64_t off = 0; off < total; off += buf.size()) {
            ctx.rng.fill(buf.data(), buf.size());
            f.pwrite(buf.data(), buf.size(), off);
        }
    }
    plat::copy_kernel(src, ctx.path("big.dst"));
    CHECK_EQ(hash_file(src), hash_file(ctx.path("big.dst")));
}

// ---------------------------------------------------------------- delete

FSB_TEST(ops, delete_file, "delete removes the name; deleting a missing file is reported") {
    write_file(ctx.path("f"), "x");
    CHECK(fs::remove(ctx.path("f")));
    CHECK(!fs::exists(ctx.path("f")));
    CHECK(!fs::remove(ctx.path("f")));  // missing -> false without error
    CHECK(fs::is_empty(ctx.root));
}

FSB_TEST(ops, delete_open_file, "deleting an open file: handle keeps working, name disappears (POSIX semantics)") {
    auto p = ctx.path("open");
    auto d = random_bytes(ctx.rng, 50000);
    write_file(p, d);
    File f = File::open(p, kRW);
    auto ec = capture_error([&] { fs::remove(p); });
    if (ec) {
        ctx.feature("Delete open file", "refused");
        ctx.unsupported("cannot delete a file that is open: " + ec.message());
    }
    bool gone = !fs::exists(p);
    bool recreate = !capture_error([&] { write_file(p, "new"); });
    std::string got(d.size(), '\0');
    CHECK_EQ(f.pread(got.data(), got.size(), 0), d.size());
    CHECK_MSG(got == d, "open handle lost data after unlink");
    f.pwrite("still-writable", 14, 0);
    ctx.feature("Delete open file", gone && recreate ? "POSIX (name freed immediately)" : "delete-pending");
}

FSB_TEST(ops, delete_readonly, "deleting a read-only file (allowed on POSIX, refused by Win32 DeleteFile)") {
    auto p = ctx.path("ro");
    write_file(p, "x");
    fs::permissions(p, fs::perms::owner_read, fs::perm_options::replace);
    auto ec = capture_error([&] { fs::remove(p); });
    if (ec) {
        CHECK(fs::exists(p));
        fs::permissions(p, fs::perms::owner_all, fs::perm_options::add);
    } else {
        CHECK(!fs::exists(p));
    }
    ctx.feature("Delete read-only file", ec ? "refused" : "allowed");
}

FSB_TEST(ops, delete_mapped_file, "deleting a file that is memory-mapped") {
    auto p = ctx.path("mapped");
    write_file(p, std::string(1 << 16, 'm'));
    File f = File::open(p, read);
    auto m = f.map(0, 1 << 16, false);
    auto ec = capture_error([&] { fs::remove(p); });
    CHECK(m.data()[100] == 'm');
    ctx.feature("Delete mapped file", ec ? "refused" : "allowed");
}

FSB_TEST(ops, delete_tree, "remove_all deletes a tree and reports the number of entries") {
    make_tree(ctx.path("t"), ctx.rng, 30, 1000, 1024);
    size_t n = count_entries(ctx.path("t")) + 1;
    CHECK_EQ(static_cast<size_t>(fs::remove_all(ctx.path("t"))), n);
    CHECK(!fs::exists(ctx.path("t")));
    CHECK_EQ(fs::remove_all(ctx.path("t")), 0u);
}

}  // namespace fsb
