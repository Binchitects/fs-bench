// Directories: create, remove, enumerate, nesting, path length.
#include "test_util.hpp"

namespace fsb {
using namespace tu;

FSB_TEST(dirs, mkdir_rmdir, "create and remove a directory; duplicates and missing parents fail") {
    auto d = ctx.path("d");
    CHECK(fs::create_directory(d));
    CHECK(fs::is_directory(d));
    CHECK(!fs::create_directory(d));  // already exists -> false, no error
    CHECK_ERR(fs::create_directory(ctx.path("missing/child")), NotFound);
    CHECK(fs::remove(d));
    CHECK(!fs::exists(d));
}

FSB_TEST(dirs, rmdir_nonempty, "removing a non-empty directory fails and leaves it intact") {
    auto d = ctx.path("full");
    fs::create_directory(d);
    write_file(d / "f", "data");
    auto ec = capture_error([&] { fs::remove(d); });
    auto k = plat::classify(ec);
    CHECK_MSG(k == ErrKind::NotEmpty || k == ErrKind::Exists, "got ", plat::err_kind_name(k));
    CHECK(read_file(d / "f") == "data");
}

FSB_TEST(dirs, nested_create, "create_directories builds a 64-level hierarchy and remove_all deletes it") {
    fs::path p = ctx.path("n");
    for (int i = 0; i < 63; ++i) p /= cat("l", i);
    CHECK(fs::create_directories(p));
    write_file(p / "leaf.txt", "leaf");
    CHECK(read_file(p / "leaf.txt") == "leaf");
    CHECK_EQ(fs::remove_all(ctx.path("n")), 65u);
    CHECK(!fs::exists(ctx.path("n")));
}

FSB_TEST(dirs, enumerate_many, "enumerating 5000 entries returns every name exactly once") {
    std::set<std::string> want;
    for (int i = 0; i < 5000; ++i) {
        std::string n = cat("entry_", i, i % 7 == 0 ? ".dat" : "");
        if (i % 500 == 0) fs::create_directory(ctx.path(n));
        else touch(ctx.path(n));
        want.insert(n);
    }
    std::vector<std::string> got;
    for (auto& e : fs::directory_iterator(ctx.root)) got.push_back(path_str(e.path().filename()));
    CHECK_EQ(got.size(), want.size());
    CHECK(std::set<std::string>(got.begin(), got.end()) == want);
}

FSB_TEST(dirs, entry_types, "directory iteration reports the correct entry types") {
    touch(ctx.path("file"));
    fs::create_directory(ctx.path("dir"));
    for (auto& e : fs::directory_iterator(ctx.root)) {
        auto n = path_str(e.path().filename());
        if (n == "file") CHECK(e.is_regular_file());
        if (n == "dir") CHECK(e.is_directory());
    }
    CHECK(fs::is_empty(ctx.path("dir")));
}

FSB_TEST(dirs, delete_while_iterating, "deleting entries during enumeration visits each entry exactly once") {
    const int n = 2000;
    for (int i = 0; i < n; ++i) touch(ctx.path(cat("x", i)));
    std::set<std::string> seen;
    int dup = 0;
    for (auto& e : fs::directory_iterator(ctx.root)) {
        if (!seen.insert(path_str(e.path().filename())).second) ++dup;
        fs::remove(e.path());
    }
    CHECK_EQ(dup, 0);
    CHECK_EQ(seen.size(), static_cast<size_t>(n));
    CHECK(fs::is_empty(ctx.root));
}

FSB_TEST(dirs, dir_link_count, "directory link count convention (2 + subdirectories on classic Unix fs)") {
    fs::create_directory(ctx.path("p"));
    for (int i = 0; i < 3; ++i) fs::create_directory(ctx.path(cat("p/s", i)));
    auto nl = plat::stat_path(ctx.path("p")).nlink;
    ctx.feature("Directory nlink", nl == 5 ? "2 + subdirs" : cat("constant (", nl, ")"));
}

FSB_TEST(dirs, long_paths, "absolute paths longer than 260 / 1100 / 3900 characters") {
    std::string comp(100, 'p');
    fs::path p = ctx.root;
    size_t longest_ok = 0;
    std::string failure;
    for (size_t target : {300u, 1100u, 3900u}) {  // absolute length; Linux PATH_MAX is 4096
        while (path_str(p).size() < target) p /= comp;
        auto ec = capture_error([&] {
            fs::create_directories(p);
            write_file(p / "f.txt", "deep");
            if (read_file(p / "f.txt") != "deep") throw std::runtime_error("content mismatch");
        });
        if (ec) {
            failure = cat(path_str(p).size(), " chars: ", ec.message());
            break;
        }
        longest_ok = path_str(p / "f.txt").size();
    }
    ctx.feature("Longest path created", cat(longest_ok, " chars"));
    if (!failure.empty()) ctx.note("limit reached at " + failure);
#ifdef _WIN32
    // Clean up bottom-up through \\?\ paths, which bypass MAX_PATH even without LongPathsEnabled.
    for (fs::path q = p; q != ctx.root && q.has_parent_path(); q = q.parent_path()) {
        std::error_code ec;
        fs::remove(fs::path(L"\\\\?\\" + (q / "f.txt").wstring()), ec);
        fs::remove(fs::path(L"\\\\?\\" + q.wstring()), ec);
    }
    const char* hint = " (enable LongPathsEnabled in the registry)";
#else
    const char* hint = "";
#endif
    CHECK_MSG(longest_ok >= 260, "cannot create paths longer than 260 characters: ", failure, hint);
}

}  // namespace fsb
