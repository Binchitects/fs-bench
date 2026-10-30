// Hard links and symbolic links.
#include "test_util.hpp"

namespace fsb {
using namespace tu;

namespace {

void hardlink_or_unsupported(TestContext& ctx, const fs::path& target, const fs::path& link) {
    std::error_code ec;
    fs::create_hard_link(target, link, ec);
    if (!ec) return;
    if (plat::is_unsupported(ec) || plat::classify(ec) == ErrKind::Permission) {
        ctx.feature("Hard links", "no");
        ctx.unsupported("hard links not supported: " + ec.message());
    }
    throw fs::filesystem_error("create_hard_link", target, link, ec);
}

}  // namespace

FSB_TEST(links, hardlink_basic, "hard links share data and identity; link count tracks them") {
    write_file(ctx.path("orig"), "shared");
    hardlink_or_unsupported(ctx, ctx.path("orig"), ctx.path("link"));
    ctx.feature("Hard links", "yes");
    CHECK_EQ(fs::hard_link_count(ctx.path("orig")), 2u);
    CHECK_EQ(plat::stat_path(ctx.path("orig")).inode, plat::stat_path(ctx.path("link")).inode);
    CHECK(fs::equivalent(ctx.path("orig"), ctx.path("link")));
    {
        File f = File::open(ctx.path("link"), write);
        f.pwrite("SHARED", 6, 0);
    }
    CHECK(read_file(ctx.path("orig")) == "SHARED");
    fs::remove(ctx.path("orig"));
    CHECK(read_file(ctx.path("link")) == "SHARED");
    CHECK_EQ(fs::hard_link_count(ctx.path("link")), 1u);
}

FSB_TEST(links, hardlink_many, "a file can have at least 1000 hard links") {
    write_file(ctx.path("base"), "b");
    hardlink_or_unsupported(ctx, ctx.path("base"), ctx.path("l0"));
    size_t made = 1;
    for (; made < 1000; ++made) {
        std::error_code ec;
        fs::create_hard_link(ctx.path("base"), ctx.path(cat("l", made)), ec);
        if (ec) {
            ctx.note(cat("link #", made + 1, " failed: ", ec.message()));
            break;
        }
    }
    CHECK_EQ(fs::hard_link_count(ctx.path("base")), made + 1);
    ctx.feature("Hard links per file (tested)", made >= 1000 ? ">= 1000" : std::to_string(made + 1));
    CHECK(made >= 1000);
}

FSB_TEST(links, hardlink_restrictions, "hard links to directories are refused; links survive rename") {
    write_file(ctx.path("f"), "x");
    hardlink_or_unsupported(ctx, ctx.path("f"), ctx.path("g"));
    fs::create_directory(ctx.path("d"));
    CHECK_THROWS(fs::create_hard_link(ctx.path("d"), ctx.path("dlink")));
    CHECK_ERR(fs::create_hard_link(ctx.path("f"), ctx.path("g")), Exists);
    plat::rename_replace(ctx.path("g"), ctx.path("h"));
    CHECK(fs::equivalent(ctx.path("f"), ctx.path("h")));
    // rename() between two links of the same file is a no-op per POSIX.
    auto ec = capture_error([&] { plat::rename_replace(ctx.path("f"), ctx.path("h")); });
    bool both = fs::exists(ctx.path("f")) && fs::exists(ctx.path("h"));
    ctx.feature("rename(link, same-inode link)", ec ? "error" : both ? "no-op (POSIX)" : "removes source");
}

FSB_TEST(links, symlink_file, "symbolic link to a file: read_symlink, follow, lstat vs stat") {
    write_file(ctx.path("target.txt"), "through-link");
    symlink_or_skip(ctx, "target.txt", ctx.path("link.txt"));
    ctx.feature("Symbolic links", "yes");
    CHECK(fs::is_symlink(ctx.path("link.txt")));
    CHECK(fs::read_symlink(ctx.path("link.txt")) == fs::path("target.txt"));
    CHECK(read_file(ctx.path("link.txt")) == "through-link");
    auto l = plat::stat_path(ctx.path("link.txt"), false);
    auto s = plat::stat_path(ctx.path("link.txt"), true);
    CHECK(l.is_symlink);
    CHECK(s.is_regular);
    CHECK_EQ(s.inode, plat::stat_path(ctx.path("target.txt")).inode);
}

FSB_TEST(links, symlink_dir, "symbolic link to a directory can be traversed and listed") {
    fs::create_directory(ctx.path("realdir"));
    write_file(ctx.path("realdir/inside"), "in");
    symlink_or_skip(ctx, "realdir", ctx.path("dirlink"), true);
    CHECK(fs::is_directory(ctx.path("dirlink")));
    CHECK(list_names(ctx.path("dirlink")) == std::set<std::string>{"inside"});
    CHECK(read_file(ctx.path("dirlink/inside")) == "in");
    // Removing the link must not touch the target's contents.
    fs::remove(ctx.path("dirlink"));
    CHECK(fs::exists(ctx.path("realdir/inside")));
}

FSB_TEST(links, symlink_dangling, "dangling links exist as links but cannot be opened") {
    symlink_or_skip(ctx, "does-not-exist", ctx.path("dangling"));
    CHECK(fs::is_symlink(ctx.path("dangling")));
    CHECK(!fs::exists(ctx.path("dangling")));
    CHECK_ERR(File::open(ctx.path("dangling"), read), NotFound);
    // Creating through a dangling link creates the target (POSIX).
    auto ec = capture_error([&] { write_file(ctx.path("dangling"), "made"); });
    if (!ec) CHECK(read_file(ctx.path("does-not-exist")) == "made");
    ctx.feature("Create through dangling symlink", ec ? "refused" : "creates target");
}

FSB_TEST(links, symlink_loop, "symlink loops are detected instead of hanging") {
    symlink_or_skip(ctx, "loop_b", ctx.path("loop_a"));
    symlink_or_skip(ctx, "loop_a", ctx.path("loop_b"));
    auto ec = capture_error([&] { File::open(ctx.path("loop_a"), read); });
    CHECK_MSG(ec, "opening a symlink loop succeeded");
    ctx.note(cat("loop error: ", plat::err_kind_name(plat::classify(ec)), " (", ec.message(), ")"));
}

FSB_TEST(links, symlink_relative_move, "relative symlinks keep working when moved together with their target") {
    fs::create_directories(ctx.path("pkg/lib"));
    write_file(ctx.path("pkg/lib/real.so"), "lib");
    symlink_or_skip(ctx, "lib/real.so", ctx.path("pkg/current.so"));
    plat::rename_replace(ctx.path("pkg"), ctx.path("pkg2"));
    CHECK(read_file(ctx.path("pkg2/current.so")) == "lib");
    // Renaming the link renames the link, not the target.
    plat::rename_replace(ctx.path("pkg2/current.so"), ctx.path("pkg2/renamed.so"));
    CHECK(fs::exists(ctx.path("pkg2/lib/real.so")));
    CHECK(fs::is_symlink(ctx.path("pkg2/renamed.so")));
}

FSB_TEST(links, symlink_long_target, "symlink with a 1000-character target") {
    std::string target;
    while (target.size() < 1000) target += "segment_0123456789/";
    target += "end";
    symlink_or_skip(ctx, u8p(target), ctx.path("longlink"));
    CHECK(path_str(fs::read_symlink(ctx.path("longlink"))) == path_str(u8p(target).make_preferred()) ||
          path_str(fs::read_symlink(ctx.path("longlink"))) == target);
}

}  // namespace fsb
