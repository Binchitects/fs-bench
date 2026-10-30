// Small helpers shared by the functional tests.
#pragma once

#include <algorithm>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "framework.hpp"

namespace fsb::tu {

using namespace fsb::plat::open_flags;
using plat::ErrKind;
using plat::File;
using plat::kCreateRW;
using plat::kCreateTruncRW;
using plat::kRW;

inline std::string random_bytes(Rng& rng, size_t n) {
    std::string s(n, '\0');
    rng.fill(s.data(), n);
    return s;
}

inline void write_file(const fs::path& p, const std::string& data) {
    File f = File::open(p, write | create | truncate);
    if (!data.empty()) f.pwrite(data.data(), data.size(), 0);
}

inline std::string read_file(const fs::path& p) {
    File f = File::open(p, read);
    std::string s(static_cast<size_t>(f.size()), '\0');
    size_t n = s.empty() ? 0 : f.pread(s.data(), s.size(), 0);
    s.resize(n);
    return s;
}

inline void touch(const fs::path& p) { File::open(p, write | create); }

inline std::set<std::string> list_names(const fs::path& dir) {
    std::set<std::string> out;
    for (auto& e : fs::directory_iterator(dir)) out.insert(path_str(e.path().filename()));
    return out;
}

inline size_t count_entries(const fs::path& dir) {
    size_t n = 0;
    for (auto it = fs::recursive_directory_iterator(dir); it != fs::recursive_directory_iterator(); ++it) ++n;
    return n;
}

// Builds a tree with `dirs` subdirectories and `files` files of varying size; returns total bytes.
inline uint64_t make_tree(const fs::path& root, Rng& rng, size_t dirs, size_t files, size_t max_size = 16384) {
    fs::create_directories(root);
    std::vector<fs::path> ds = {root};
    for (size_t i = 0; i < dirs; ++i) {
        fs::path d = ds[rng.below(ds.size())] / cat("d", i);
        fs::create_directory(d);
        ds.push_back(d);
    }
    uint64_t total = 0;
    for (size_t i = 0; i < files; ++i) {
        size_t sz = rng.below(max_size + 1);
        write_file(ds[rng.below(ds.size())] / cat("f", i, ".bin"), random_bytes(rng, sz));
        total += sz;
    }
    return total;
}

// Compares two trees: same relative paths, types and file contents.
inline bool trees_equal(const fs::path& a, const fs::path& b, std::string* why = nullptr) {
    auto collect = [](const fs::path& r) {
        std::set<std::string> s;
        for (auto it = fs::recursive_directory_iterator(r); it != fs::recursive_directory_iterator(); ++it)
            s.insert(path_str(fs::relative(it->path(), r)));
        return s;
    };
    auto sa = collect(a), sb = collect(b);
    if (sa != sb) {
        if (why) *why = cat("entry sets differ (", sa.size(), " vs ", sb.size(), ")");
        return false;
    }
    for (auto& rel : sa) {
        fs::path pa = a / u8p(rel), pb = b / u8p(rel);
        if (fs::is_symlink(pa) != fs::is_symlink(pb) || fs::is_directory(pa) != fs::is_directory(pb)) {
            if (why) *why = "type differs: " + rel;
            return false;
        }
        if (fs::is_regular_file(fs::symlink_status(pa)) && !files_equal(pa, pb)) {
            if (why) *why = "content differs: " + rel;
            return false;
        }
    }
    return true;
}

inline void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

inline std::string resolution_name(int64_t ns) {
    if (ns <= 1) return "1 ns";
    if (ns < 1000) return cat(ns, " ns");
    if (ns < 1000000) return cat(ns / 1000, " us");
    if (ns < 1000000000) return cat(ns / 1000000, " ms");
    return cat(ns / 1000000000, " s");
}

// Tries to create a symlink; turns "no privilege" (Windows without Developer Mode) into a skip.
inline void symlink_or_skip(TestContext& ctx, const fs::path& target, const fs::path& link, bool dir = false) {
    std::error_code ec;
    if (dir) fs::create_directory_symlink(target, link, ec);
    else fs::create_symlink(target, link, ec);
    if (!ec) return;
    if (plat::is_permission_error(ec))
        ctx.skip("creating symlinks needs privilege here (Windows: enable Developer Mode or run elevated)");
    if (plat::is_unsupported(ec)) ctx.unsupported("symbolic links not supported: " + ec.message());
    throw fs::filesystem_error("create_symlink", target, link, ec);
}

}  // namespace fsb::tu
