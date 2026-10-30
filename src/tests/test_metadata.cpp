// Metadata: stat fields, timestamps, permissions, file identity, space accounting.
#include "test_util.hpp"

namespace fsb {
using namespace tu;

namespace {
constexpr int64_t kSec = 1000000000LL;
constexpr int64_t kY2000 = 946684800LL * kSec;  // 2000-01-01T00:00:00Z
}  // namespace

FSB_TEST(metadata, stat_basics, "stat reports type, size, link count, inode and device consistently") {
    auto p = ctx.path("f.bin");
    write_file(p, std::string(12345, 'x'));
    auto st = plat::stat_path(p);
    CHECK(st.is_regular);
    CHECK(!st.is_dir);
    CHECK_EQ(st.size, 12345u);
    CHECK_EQ(st.nlink, 1u);
    CHECK(st.allocated >= 12345u || st.allocated == 0);  // 0 possible for inline/compressed data
    auto ds = plat::stat_path(ctx.root);
    CHECK(ds.is_dir);
    CHECK_EQ(st.device, ds.device);
    File f = File::open(p, read);
    auto hs = f.stat();
    CHECK_EQ(hs.inode, st.inode);
    CHECK_EQ(hs.size, st.size);
    ctx.note(cat("12345-byte file allocates ", st.allocated, " bytes"));
}

FSB_TEST(metadata, mtime_on_write, "modification time advances when the file is written or truncated") {
    auto p = ctx.path("m.bin");
    write_file(p, "a");
    plat::set_file_times(p, kY2000, kY2000);
    CHECK_EQ(plat::stat_path(p).mtime_ns, kY2000);
    {
        File f = File::open(p, write);
        f.pwrite("b", 1, 0);
    }
    auto m1 = plat::stat_path(p).mtime_ns;
    CHECK_MSG(m1 > kY2000, "mtime not updated by write");
    plat::set_file_times(p, kY2000, kY2000);
    {
        File f = File::open(p, write);
        f.truncate(100);
    }
    CHECK_MSG(plat::stat_path(p).mtime_ns > kY2000, "mtime not updated by truncate");
}

FSB_TEST(metadata, dir_mtime_on_change, "directory mtime advances when entries are added/removed/renamed") {
    auto d = ctx.path("dir");
    fs::create_directory(d);
    auto step = [&](const char* what, auto&& fn) {
        plat::set_file_times(d, kY2000, kY2000);
        fn();
        CHECK_MSG(plat::stat_path(d).mtime_ns > kY2000, "directory mtime not updated on ", what);
    };
    step("create", [&] { touch(d / "a"); });
    step("rename", [&] { plat::rename_replace(d / "a", d / "b"); });
    step("delete", [&] { fs::remove(d / "b"); });
}

FSB_TEST(metadata, timestamp_resolution, "set/get timestamps; detect stored resolution and range") {
    auto p = ctx.path("ts.bin");
    write_file(p, "x");
    const int64_t t = 1234567890LL * kSec + 123456789;  // 2009-02-13 with ns digits
    plat::set_file_times(p, t, t);
    auto st = plat::stat_path(p);
    int64_t diff = t - st.mtime_ns;
    CHECK_MSG(diff >= 0 && diff < 3 * kSec, "mtime off by ", diff, " ns");
    int64_t res = 1;
    for (int64_t r : {int64_t(1), int64_t(100), int64_t(1000), int64_t(1000000), int64_t(10000000), kSec, 2 * kSec})
        if (st.mtime_ns % r == 0) res = r;
    if (diff == 0) res = 1;
    ctx.feature("mtime resolution", resolution_name(res));

    // Range: pre-1970 and post-2038 timestamps.
    auto try_time = [&](int64_t when) {
        auto ec = capture_error([&] { plat::set_file_times(p, when, when); });
        if (ec) return false;
        auto back = plat::stat_path(p).mtime_ns;
        return back <= when && when - back < 3 * kSec;
    };
    bool pre1970 = try_time(-315619200LL * kSec);  // 1960-01-01
    bool post2038 = try_time(4102444800LL * kSec);  // 2100-01-01
    ctx.feature("Timestamps before 1970", pre1970 ? "yes" : "no");
    ctx.feature("Timestamps after 2038", post2038 ? "yes" : "no");
}

FSB_TEST(metadata, atime_policy, "classify access-time updates (strictatime / relatime / noatime)") {
    auto p = ctx.path("a.bin");
    write_file(p, std::string(4096, 'a'));
    auto read_once = [&] {
        File f = File::open(p, read);
        char b[16];
        f.pread(b, sizeof(b), 0);
    };
    // atime far older than mtime: relatime and strictatime both update.
    auto mt = plat::stat_path(p).mtime_ns;
    plat::set_file_times(p, kY2000, mt);
    plat::drop_file_cache(p);
    read_once();
    bool upd_old = plat::stat_path(p).atime_ns > kY2000;
    // atime newer than mtime and recent: only strictatime updates.
    auto now_ns = plat::stat_path(p).ctime_ns;
    int64_t recent = now_ns - 60 * kSec;
    plat::set_file_times(p, recent, kY2000);
    sleep_ms(20);
    read_once();
    bool upd_recent = plat::stat_path(p).atime_ns > recent;
    std::string policy = !upd_old ? "noatime (or deferred)" : upd_recent ? "strictatime" : "relatime";
    ctx.feature("atime updates", policy);
}

FSB_TEST(metadata, birth_time, "creation (birth) time is reported and is not changed by writes") {
    auto p = ctx.path("b.bin");
    write_file(p, "x");
    auto st = plat::stat_path(p);
    if (!st.btime_ns) {
        ctx.feature("Birth time", "no");
        ctx.unsupported("filesystem/kernel does not report birth time");
    }
    ctx.feature("Birth time", "yes");
    auto b0 = *st.btime_ns;
    CHECK_MSG(b0 <= st.mtime_ns + kSec, "btime later than mtime");
    sleep_ms(20);
    {
        File f = File::open(p, write);
        f.pwrite("yy", 2, 0);
    }
    CHECK_EQ(*plat::stat_path(p).btime_ns, b0);
}

FSB_TEST(metadata, ctime_on_change, "change time advances on metadata changes (rename, chmod)") {
    auto p = ctx.path("c.bin");
    write_file(p, "x");
    auto c0 = plat::stat_path(p).ctime_ns;
    sleep_ms(30);
    fs::permissions(p, fs::perms::owner_read, fs::perm_options::replace);
    auto c1 = plat::stat_path(p).ctime_ns;
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    sleep_ms(30);
    plat::rename_replace(p, ctx.path("c2.bin"));
    auto c2 = plat::stat_path(ctx.path("c2.bin")).ctime_ns;
    CHECK_MSG(c1 > c0, "ctime unchanged after permission change");
    ctx.feature("ctime updated on rename", c2 > c1 ? "yes" : "no");
}

FSB_TEST(metadata, readonly_enforced, "a read-only file cannot be opened for writing") {
#ifndef _WIN32
    if (plat::is_privileged()) ctx.skip("running as root: permission checks are bypassed");
#endif
    auto p = ctx.path("ro.bin");
    write_file(p, "keep");
    fs::permissions(p, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                    fs::perm_options::remove);
    auto ec = capture_error([&] { File::open(p, write); });
    fs::permissions(p, fs::perms::owner_write, fs::perm_options::add);
    if (!ec) {
        ctx.feature("Read-only enforcement", "no");
        ctx.unsupported("filesystem does not enforce the read-only permission");
    }
    CHECK_EQ(plat::classify(ec), ErrKind::Permission);
    CHECK(read_file(p) == "keep");
    ctx.feature("Read-only enforcement", "yes");
}

FSB_TEST(metadata, posix_mode_bits, "POSIX permission bits are stored exactly") {
#ifdef _WIN32
    ctx.feature("POSIX mode bits", "no (read-only attribute only)");
    ctx.unsupported("Windows only maps the read-only attribute");
#else
    auto p = ctx.path("mode.bin");
    write_file(p, "x");
    bool all_ok = true;
    for (unsigned m : {0640u, 0600u, 0755u, 0444u, 0000u, 04755u & 0777u, 0777u}) {
        auto ec = capture_error([&] { fs::permissions(p, static_cast<fs::perms>(m), fs::perm_options::replace); });
        auto got = plat::stat_path(p).mode & 0777;
        if (ec || got != m) {
            all_ok = false;
            ctx.note(cat("mode ", std::oct, m, " -> ", got, std::dec, ec ? " (" + ec.message() + ")" : ""));
        }
    }
    fs::permissions(p, fs::perms::owner_all, fs::perm_options::replace);
    ctx.feature("POSIX mode bits", all_ok ? "yes" : "no");
    if (!all_ok) ctx.unsupported("filesystem does not persist POSIX mode bits");
#endif
}

FSB_TEST(metadata, file_identity, "file id (inode) is stable across rename and unique per file") {
    auto a = ctx.path("a"), b = ctx.path("b");
    touch(a);
    touch(b);
    auto ia = plat::stat_path(a).inode, ib = plat::stat_path(b).inode;
    CHECK(ia != ib);
    fs::create_directory(ctx.path("sub"));
    plat::rename_replace(a, ctx.path("sub/a2"));
    CHECK_EQ(plat::stat_path(ctx.path("sub/a2")).inode, ia);
}

FSB_TEST(metadata, space_accounting, "free-space and allocation reporting react to writes and deletes") {
    auto info0 = plat::fs_info(ctx.root);
    CHECK(info0.total_bytes > 0);
    CHECK(info0.avail_bytes <= info0.total_bytes);
    CHECK(info0.free_bytes <= info0.total_bytes);
    auto p = ctx.path("space.bin");
    const uint64_t sz = 64ull << 20;
    {
        File f = File::open(p, write | create | truncate);
        std::string buf(1 << 20, '\0');
        for (uint64_t off = 0; off < sz; off += buf.size()) {
            ctx.rng.fill(buf.data(), buf.size());
            f.pwrite(buf.data(), buf.size(), off);
        }
        f.sync();
    }
    auto st = plat::stat_path(p);
    CHECK_MSG(st.allocated >= sz * 9 / 10 || st.allocated == 0, "allocated ", st.allocated, " < size ", sz);
    auto info1 = plat::fs_info(ctx.root);
    int64_t delta = static_cast<int64_t>(info0.avail_bytes) - static_cast<int64_t>(info1.avail_bytes);
    ctx.note(cat("free space dropped by ", fmt_bytes(static_cast<double>(delta)), " after writing 64 MiB (other "
                 "activity on the volume affects this)"));
    ctx.feature("Block size", std::to_string(info0.block_size));
}

}  // namespace fsb
