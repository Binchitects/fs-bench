// Advanced filesystem features: sparse files, hole punching, preallocation,
// extended attributes / data streams, byte-range locks, change notification.
#include <cstring>

#include "test_util.hpp"

namespace fsb {
using namespace tu;

FSB_TEST(features, sparse_files, "sparse files: holes take no space, read as zeros, and are reported") {
    auto p = ctx.path("sparse.bin");
    File f = File::open(p, kCreateTruncRW);
    std::error_code ec;
    f.set_sparse(ec);
    if (ec) {
        ctx.feature("Sparse files", "no");
        ctx.unsupported("cannot mark file sparse: " + ec.message());
    }
    auto d = random_bytes(ctx.rng, 64 << 10);
    f.pwrite(d.data(), d.size(), 0);
    f.pwrite(d.data(), d.size(), 1ull << 30);
    f.sync();
    auto st = f.stat();
    CHECK_EQ(st.size, (1ull << 30) + d.size());
    std::string hole(1 << 20, '\x11');
    CHECK_EQ(f.pread(hole.data(), hole.size(), 512ull << 20), hole.size());
    CHECK(hole.find_first_not_of('\0') == std::string::npos);
    bool sparse = st.allocated < (64ull << 20);
    ctx.feature("Sparse files", sparse ? "yes" : "no");
    ctx.note(cat("1 GiB logical, ", fmt_bytes(static_cast<double>(st.allocated)), " allocated"));
    auto ranges = f.data_ranges(ec);
    if (ec) ctx.feature("Hole/data query (SEEK_HOLE)", "no");
    else {
        ctx.feature("Hole/data query (SEEK_HOLE)", ranges.size() >= 2 ? "yes" : cat("coarse (", ranges.size(), " ranges)"));
        ctx.note(cat(ranges.size(), " data ranges reported"));
    }
    if (!sparse) ctx.unsupported("filesystem allocated the holes");
}

FSB_TEST(features, punch_hole, "punching a hole deallocates space and reads back zeros") {
    auto p = ctx.path("punch.bin");
    const uint64_t sz = 16ull << 20;
    File f = File::open(p, kCreateTruncRW);
    std::error_code ec;
    f.set_sparse(ec);
    auto d = random_bytes(ctx.rng, static_cast<size_t>(sz));
    f.pwrite(d.data(), d.size(), 0);
    f.sync();
    auto before = f.stat().allocated;
    f.punch_hole(4ull << 20, 8ull << 20, ec);
    if (ec) {
        ctx.feature("Hole punching", "no");
        ctx.unsupported("hole punching not supported: " + ec.message());
    }
    f.sync();
    auto after = f.stat().allocated;
    CHECK_EQ(f.size(), sz);
    std::string got(static_cast<size_t>(sz), '\0');
    CHECK_EQ(f.pread(got.data(), got.size(), 0), got.size());
    CHECK(got.compare(0, 4 << 20, d, 0, 4 << 20) == 0);
    CHECK(got.find_first_not_of('\0', 4 << 20) == (12u << 20));
    CHECK(got.compare(12 << 20, 4 << 20, d, 12 << 20, 4 << 20) == 0);
    ctx.note(cat("allocated ", fmt_bytes(static_cast<double>(before)), " -> ", fmt_bytes(static_cast<double>(after))));
    ctx.feature("Hole punching", after < before ? "yes (deallocates)" : "zeroes only");
}

FSB_TEST(features, preallocate, "preallocation reserves space without changing the file size") {
    auto p = ctx.path("prealloc.bin");
    File f = File::open(p, kCreateTruncRW);
    std::error_code ec;
    const uint64_t len = 64ull << 20;
    f.preallocate(0, len, ec);
    if (ec) {
        ctx.feature("Preallocation", "no");
        ctx.unsupported("preallocation not supported: " + ec.message());
    }
    auto st = f.stat();
    CHECK_EQ(st.size, 0u);
    ctx.note(cat("allocated after reserving 64 MiB: ", fmt_bytes(static_cast<double>(st.allocated))));
    ctx.feature("Preallocation", st.allocated >= len ? "yes" : "accepted, not reflected");
    // Writing into the reserved range works and extends the size.
    auto d = random_bytes(ctx.rng, 1 << 20);
    f.pwrite(d.data(), d.size(), 10 << 20);
    CHECK_EQ(f.size(), (11u << 20));
}

FSB_TEST(features, xattrs, "extended attributes (Linux) / alternate data streams (Windows)") {
    auto p = ctx.path("x.bin");
    write_file(p, "main-data");
    std::error_code ec;
    plat::xattr_set(p, "fsbench.one", "value-1", ec);
    if (ec) {
        ctx.feature("Extended attributes", "no");
        ctx.unsupported(plat::xattr_backend() + " not supported: " + ec.message());
    }
    ctx.feature("Extended attributes", plat::xattr_backend());
    CHECK(plat::xattr_get(p, "fsbench.one", ec) == "value-1" && !ec);
    plat::xattr_set(p, "fsbench.one", "overwritten", ec);
    CHECK(!ec);
    CHECK(plat::xattr_get(p, "fsbench.one", ec) == "overwritten");
    plat::xattr_set(p, "fsbench.empty", "", ec);
    CHECK(!ec);
    CHECK(plat::xattr_get(p, "fsbench.empty", ec).empty() && !ec);
    auto names = plat::xattr_list(p, ec);
    CHECK(!ec);
    CHECK(std::count(names.begin(), names.end(), "fsbench.one") == 1);
    CHECK(std::count(names.begin(), names.end(), "fsbench.empty") == 1);
    plat::xattr_remove(p, "fsbench.one", ec);
    CHECK(!ec);
    plat::xattr_get(p, "fsbench.one", ec);
    CHECK_MSG(ec, "removed attribute is still readable");
    CHECK(read_file(p) == "main-data");  // main data untouched

    // Largest single value: probe 1 KiB .. 1 MiB.
    size_t largest = 0;
    for (size_t sz = 1024; sz <= (1u << 20); sz *= 4) {
        auto v = random_bytes(ctx.rng, sz);
        plat::xattr_set(p, "fsbench.big", v, ec);
        if (ec || plat::xattr_get(p, "fsbench.big", ec) != v) break;
        largest = sz;
    }
    ctx.feature("Largest xattr value (tested)", largest ? fmt_bytes(static_cast<double>(largest)) : "< 1 KiB");
    // Many attributes on one file.
    int many = 0;
    for (; many < 200; ++many) {
        plat::xattr_set(p, cat("fsbench.n", many), "v", ec);
        if (ec) break;
    }
    ctx.feature("xattrs per file (tested)", many >= 200 ? ">= 200" : std::to_string(many));
    // Attributes follow the file through rename and copy.
    plat::rename_replace(p, ctx.path("renamed.bin"));
    CHECK(plat::xattr_get(ctx.path("renamed.bin"), "fsbench.empty", ec).empty() && !ec);
}

FSB_TEST(features, byte_range_locks, "byte-range locks: exclusive vs shared, overlap, unlock") {
    auto p = ctx.path("lock.bin");
    write_file(p, std::string(8192, 'L'));
    File a = File::open(p, kRW);
    File b = File::open(p, kRW);
    auto ec = capture_error([&] { a.try_lock(0, 100, true); });
    if (ec) {
        ctx.feature("Byte-range locks", "no");
        ctx.unsupported("locking not supported: " + ec.message());
    }
    CHECK_MSG(!b.try_lock(50, 10, true), "overlapping exclusive lock granted");
    CHECK_MSG(!b.try_lock(0, 1, false), "shared lock granted over exclusive range");
    CHECK_MSG(b.try_lock(100, 100, true), "adjacent non-overlapping lock refused");
    b.unlock(100, 100);
    a.unlock(0, 100);
    CHECK_MSG(b.try_lock(50, 10, true), "lock refused after unlock");
    b.unlock(50, 10);
    // Shared locks coexist; an exclusive lock is refused while they are held.
    CHECK(a.try_lock(1000, 100, false));
    CHECK(b.try_lock(1000, 100, false));
    File c = File::open(p, kRW);
    CHECK_MSG(!c.try_lock(1000, 1, true), "exclusive lock granted over shared locks");
    a.unlock(1000, 100);
    b.unlock(1000, 100);
    // Locks are released when the handle closes.
    CHECK(a.try_lock(0, 8192, true));
    a.close();
    CHECK_MSG(c.try_lock(0, 8192, true), "lock not released on close");
    ctx.feature("Byte-range locks", "yes");
}

FSB_TEST(features, change_notification, "directory change notifications (inotify / ReadDirectoryChangesW)") {
    plat::DirWatcher w;
    std::error_code ec;
    w.start(ctx.root, ec);
    if (ec) {
        ctx.feature("Change notification", "no");
        ctx.unsupported("cannot watch directory: " + ec.message());
    }
    write_file(ctx.path("watched.txt"), "x");
    std::vector<std::string> events;
    Timer t;
    while (t.elapsed_s() < 3) {
        auto e = w.wait(200);
        events.insert(events.end(), e.begin(), e.end());
        if (std::count(events.begin(), events.end(), "watched.txt")) break;
    }
    bool seen = std::count(events.begin(), events.end(), "watched.txt") > 0;
    ctx.feature("Change notification", seen ? cat("yes (", fmt_duration_ns(static_cast<double>(t.elapsed_ns())), ")") : "no events");
    if (!seen) ctx.unsupported(cat("no event within 3 s (", events.size(), " other events)"));
}

FSB_TEST(features, volume_info, "volume capacity/type information is sane") {
    auto info = plat::fs_info(ctx.root);
    CHECK(!info.type.empty());
    CHECK(info.total_bytes > 0);
    CHECK(info.avail_bytes <= info.total_bytes);
    CHECK(info.block_size > 0);
    ctx.feature("Filesystem type", info.type);
    ctx.feature("Max name length (reported)", std::to_string(info.max_name_len));
}

}  // namespace fsb
