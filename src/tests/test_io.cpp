// Basic file I/O semantics: create, read, write, append, truncate, sync, mmap, direct I/O.
#include <cstring>

#include "test_util.hpp"

namespace fsb {
using namespace tu;

FSB_TEST(io, create_empty, "create an empty file; it exists with size 0") {
    auto p = ctx.path("empty.bin");
    { File f = File::open(p, write | create | exclusive); }
    CHECK(fs::exists(p));
    CHECK(fs::is_regular_file(p));
    CHECK_EQ(fs::file_size(p), 0u);
    CHECK_EQ(plat::stat_path(p).size, 0u);
}

FSB_TEST(io, write_read_roundtrip, "write random data of many sizes and read it back byte-exact") {
    for (size_t sz : {size_t(0), size_t(1), size_t(511), size_t(4096), size_t(65537), size_t(1 << 20) + 3, size_t(8 << 20)}) {
        auto p = ctx.path(cat("rt_", sz));
        auto data = random_bytes(ctx.rng, sz);
        write_file(p, data);
        CHECK_EQ(fs::file_size(p), sz);
        CHECK_MSG(read_file(p) == data, "content mismatch for size ", sz);
    }
}

FSB_TEST(io, positional_io, "random overlapping pwrite/pread against an in-memory model") {
    auto p = ctx.path("model.bin");
    File f = File::open(p, kCreateTruncRW);
    std::string model;
    for (int i = 0; i < 2000; ++i) {
        uint64_t off = ctx.rng.below(2 << 20);
        size_t len = 1 + ctx.rng.below(16384);
        auto chunk = random_bytes(ctx.rng, len);
        f.pwrite(chunk.data(), len, off);
        if (model.size() < off + len) model.resize(off + len, '\0');
        std::memcpy(model.data() + off, chunk.data(), len);
        if (i % 50 == 0) {  // spot-check a random region
            uint64_t ro = ctx.rng.below(model.size());
            size_t rl = static_cast<size_t>(std::min<uint64_t>(1 + ctx.rng.below(65536), model.size() - ro));
            std::string got(rl, '\0');
            CHECK_EQ(f.pread(got.data(), rl, ro), rl);
            CHECK_MSG(std::memcmp(got.data(), model.data() + ro, rl) == 0, "mismatch at ", ro, "+", rl);
        }
    }
    CHECK_EQ(f.size(), model.size());
    f.close();
    CHECK_MSG(read_file(p) == model, "final content differs from model");
}

FSB_TEST(io, sequential_cursor, "read()/write()/seek() move the file position correctly") {
    auto p = ctx.path("cursor.bin");
    File f = File::open(p, kCreateTruncRW);
    f.write("hello", 5);
    f.write(" world", 6);
    CHECK_EQ(f.seek(0, 1), 11u);
    CHECK_EQ(f.seek(0, 0), 0u);
    char buf[16] = {};
    CHECK_EQ(f.read(buf, 5), 5u);
    CHECK(std::string(buf, 5) == "hello");
    CHECK_EQ(f.seek(-5, 2), 6u);
    CHECK_EQ(f.read(buf, 16), 5u);
    CHECK(std::string(buf, 5) == "world");
    CHECK_EQ(f.read(buf, 16), 0u);  // at EOF
}

FSB_TEST(io, append_mode, "append-mode writes always land at EOF, even after other writers extend the file") {
    auto p = ctx.path("append.log");
    write_file(p, "0123456789");
    File a = File::open(p, append);
    File other = File::open(p, write);
    a.write("AAAA", 4);
    other.pwrite("BBBBBB", 6, 14);  // extend to 20 bytes behind the appender's back
    a.write("CCCC", 4);
    a.close();
    other.close();
    auto s = read_file(p);
    CHECK_EQ(s.size(), 24u);
    CHECK(s.substr(0, 14) == "0123456789AAAA");
    CHECK(s.substr(14, 6) == "BBBBBB");
    CHECK(s.substr(20) == "CCCC");
}

FSB_TEST(io, exclusive_create, "exclusive create fails if the file already exists") {
    auto p = ctx.path("excl");
    { File f = File::open(p, write | create | exclusive); }
    CHECK_ERR(File::open(p, write | create | exclusive), Exists);
}

FSB_TEST(io, open_missing, "opening a missing file fails with not-found; create in a missing dir fails") {
    CHECK_ERR(File::open(ctx.path("nope"), read), NotFound);
    CHECK_ERR(File::open(ctx.path("no_dir/file"), write | create), NotFound);
}

FSB_TEST(io, truncate_shrink_grow, "truncate shrinks keeping the prefix and grows with zero fill") {
    auto p = ctx.path("trunc.bin");
    auto data = random_bytes(ctx.rng, 100000);
    write_file(p, data);
    File f = File::open(p, kRW);
    f.truncate(1000);
    CHECK_EQ(f.size(), 1000u);
    f.truncate(50000);
    CHECK_EQ(f.size(), 50000u);
    std::string got(50000, '\x55');
    CHECK_EQ(f.pread(got.data(), got.size(), 0), 50000u);
    CHECK(got.substr(0, 1000) == data.substr(0, 1000));
    CHECK(got.find_first_not_of('\0', 1000) == std::string::npos);
    f.truncate(0);
    CHECK_EQ(f.size(), 0u);
}

FSB_TEST(io, truncate_on_open, "opening with truncate discards existing content") {
    auto p = ctx.path("t.bin");
    write_file(p, random_bytes(ctx.rng, 12345));
    { File f = File::open(p, write | truncate); }
    CHECK_EQ(fs::file_size(p), 0u);
}

FSB_TEST(io, write_past_eof, "writing beyond EOF creates a zero-filled gap") {
    auto p = ctx.path("gap.bin");
    File f = File::open(p, kCreateTruncRW);
    f.pwrite("X", 1, 3 << 20);
    CHECK_EQ(f.size(), (3u << 20) + 1);
    std::string gap(3 << 20, '\x7f');
    CHECK_EQ(f.pread(gap.data(), gap.size(), 0), gap.size());
    CHECK(gap.find_first_not_of('\0') == std::string::npos);
}

FSB_TEST(io, read_past_eof, "reads at/after EOF return short or zero counts") {
    auto p = ctx.path("short.bin");
    write_file(p, "abc");
    File f = File::open(p, read);
    char buf[10];
    CHECK_EQ(f.pread(buf, 10, 0), 3u);
    CHECK_EQ(f.pread(buf, 10, 3), 0u);
    CHECK_EQ(f.pread(buf, 10, 1000), 0u);
}

FSB_TEST(io, multi_handle_coherence, "data written via one handle is immediately visible via another") {
    auto p = ctx.path("coh.bin");
    File w = File::open(p, kCreateTruncRW);
    File r = File::open(p, read);
    for (int i = 0; i < 200; ++i) {
        auto d = random_bytes(ctx.rng, 4096);
        uint64_t off = ctx.rng.below(256) * 4096;
        w.pwrite(d.data(), d.size(), off);
        std::string got(4096, '\0');
        CHECK_EQ(r.pread(got.data(), got.size(), off), 4096u);
        CHECK(got == d);
    }
}

FSB_TEST(io, fsync_durability_api, "fsync/fdatasync/directory fsync succeed and data survives reopen") {
    auto p = ctx.path("durable.bin");
    auto d = random_bytes(ctx.rng, 1 << 20);
    {
        File f = File::open(p, write | create | truncate);
        f.pwrite(d.data(), d.size(), 0);
        f.datasync();
        f.sync();
    }
    std::error_code ec;
    File dir = File::open_dir(ctx.root, ec);
    if (!ec) {
        auto e = capture_error([&] { dir.sync(); });
        ctx.feature("Directory fsync", e ? "no (" + std::string(plat::err_kind_name(plat::classify(e))) + ")" : "yes");
    } else {
        ctx.feature("Directory fsync", "cannot open dir handle");
    }
    CHECK(read_file(p) == d);
}

FSB_TEST(io, sync_write_flags, "O_SYNC / O_DSYNC (write-through) writes work") {
    for (unsigned fl : {plat::open_flags::sync, plat::open_flags::dsync}) {
        auto p = ctx.path(cat("sync_", fl));
        auto d = random_bytes(ctx.rng, 64 << 10);
        {
            File f = File::open(p, write | create | truncate | fl);
            for (size_t off = 0; off < d.size(); off += 4096) f.pwrite(d.data() + off, 4096, off);
        }
        CHECK(read_file(p) == d);
    }
}

FSB_TEST(io, large_file_4g, "files larger than 4 GiB (offsets beyond 32 bits)") {
    auto p = ctx.path("big.bin");
    File f = File::open(p, kCreateTruncRW);
    std::error_code sec;
    f.set_sparse(sec);  // keep it cheap where possible
    const uint64_t off = 5ull << 30;
    auto d = random_bytes(ctx.rng, 4096);
    auto ec = capture_error([&] { f.pwrite(d.data(), d.size(), off); });
    if (plat::classify(ec) == ErrKind::TooBig || plat::classify(ec) == ErrKind::InvalidArg) {
        ctx.feature("Files > 4 GiB", "no");
        ctx.unsupported("filesystem limits file size below 5 GiB (" + ec.message() + ")");
    }
    if (plat::classify(ec) == ErrKind::NoSpace) ctx.skip("not enough free space for a 5 GiB non-sparse file");
    if (ec) throw std::system_error(ec);
    CHECK_EQ(f.size(), off + 4096);
    std::string got(4096, '\0');
    CHECK_EQ(f.pread(got.data(), 4096, off), 4096u);
    CHECK(got == d);
    ctx.feature("Files > 4 GiB", "yes");
    ctx.note(cat("allocated after 4 KiB write at 5 GiB: ", fmt_bytes(static_cast<double>(f.stat().allocated))));
}

FSB_TEST(io, integrity_stress, "64 MiB written in random-sized chunks verifies by hash after reopen") {
    auto p = ctx.path("stress.bin");
    const uint64_t total = 64ull << 20;
    const uint64_t seed = ctx.rng.next();
    {
        File f = File::open(p, write | create | truncate);
        std::vector<unsigned char> buf(1 << 20);
        for (uint64_t off = 0; off < total;) {
            size_t n = static_cast<size_t>(std::min<uint64_t>(1 + ctx.rng.below(buf.size()), total - off));
            pattern_fill(buf.data(), n, seed, off);
            f.pwrite(buf.data(), n, off);
            off += n;
        }
        f.sync();
    }
    CHECK_EQ(fs::file_size(p), total);
    File f = File::open(p, read);
    std::vector<unsigned char> got(1 << 20), want(1 << 20);
    for (uint64_t off = 0; off < total; off += got.size()) {
        CHECK_EQ(f.pread(got.data(), got.size(), off), got.size());
        pattern_fill(want.data(), want.size(), seed, off);
        CHECK_MSG(got == want, "corruption in block at offset ", off);
    }
}

FSB_TEST(io, mmap_coherence, "memory-mapped writes are visible to read() and vice versa") {
    auto p = ctx.path("map.bin");
    const size_t len = 1 << 20;
    File f = File::open(p, kCreateTruncRW);
    f.truncate(len);
    {
        auto m = f.map(0, len, true);
        auto d = random_bytes(ctx.rng, len);
        std::memcpy(m.data(), d.data(), len);
        m.flush();
        std::string got(len, '\0');
        CHECK_EQ(f.pread(got.data(), len, 0), len);
        CHECK_MSG(got == d, "pread does not see mmap writes");
        auto d2 = random_bytes(ctx.rng, 4096);
        f.pwrite(d2.data(), d2.size(), 8192);
        CHECK_MSG(std::memcmp(m.data() + 8192, d2.data(), d2.size()) == 0, "mapping does not see pwrite");
    }
    ctx.feature("mmap shared coherence", "yes");
}

FSB_TEST(io, direct_io, "direct (unbuffered) I/O with aligned buffers; behaviour of unaligned I/O") {
    auto p = ctx.path("direct.bin");
    std::error_code ec;
    File f = File::open(p, kCreateTruncRW | plat::open_flags::direct, ec);
    if (ec) {
        ctx.feature("Direct I/O", "no");
        ctx.unsupported("cannot open with O_DIRECT/NO_BUFFERING: " + ec.message());
    }
    const size_t align = plat::direct_io_alignment(ctx.root);
    AlignedBuffer w(256 << 10, align), r(256 << 10, align);
    ctx.rng.fill(w.data(), w.size());
    auto e = capture_error([&] { f.pwrite(w.data(), w.size(), 0); });
    if (e) {
        ctx.feature("Direct I/O", "no");
        ctx.unsupported("aligned direct write failed: " + e.message());
    }
    CHECK_EQ(f.pread(r.data(), r.size(), 0), r.size());
    CHECK(std::memcmp(w.data(), r.data(), w.size()) == 0);
    ctx.feature("Direct I/O", cat("yes (align ", align, ")"));
    // Unaligned offset: most filesystems reject it, some silently fall back to buffered I/O.
    auto ue = capture_error([&] { f.pwrite(w.data(), 4096, 100); });
    ctx.note(std::string("unaligned direct write: ") + (ue ? "rejected (" + ue.message() + ")" : "accepted"));
    f.close();
    // Buffered view must agree with what was written directly.
    CHECK_EQ(read_file(p).substr(0, 100), std::string(reinterpret_cast<char*>(w.data()), 100));
}

FSB_TEST(io, anonymous_tmpfile, "unnamed temporary files (O_TMPFILE / delete-on-close)") {
    std::error_code ec;
    File f = File::open_temp_anonymous(ctx.root, ec);
    if (ec) {
        ctx.feature("Anonymous temp files", "no");
        ctx.unsupported("O_TMPFILE / delete-on-close not supported: " + ec.message());
    }
    auto d = random_bytes(ctx.rng, 10000);
    f.pwrite(d.data(), d.size(), 0);
    std::string got(d.size(), '\0');
    CHECK_EQ(f.pread(got.data(), got.size(), 0), d.size());
    CHECK(got == d);
#ifdef _WIN32
    fs::path visible = f.path();
    f.close();
    CHECK_MSG(!fs::exists(visible), "delete-on-close file still exists after close");
    ctx.feature("Anonymous temp files", "delete-on-close");
#else
    CHECK_EQ(list_names(ctx.root).size(), 0u);  // never visible in the namespace
    auto named = ctx.path("materialized.bin");
    f.link_to(named, ec);
    if (ec) ctx.note("linking O_TMPFILE into namespace failed: " + ec.message());
    else CHECK(read_file(named) == d);
    ctx.feature("Anonymous temp files", ec ? "O_TMPFILE (no linkat)" : "O_TMPFILE + linkat");
#endif
}

}  // namespace fsb
