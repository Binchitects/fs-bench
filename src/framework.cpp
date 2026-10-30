#include "framework.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <thread>

#include "report.hpp"

namespace fsb {

// ---------------------------------------------------------------- profiles

void apply_profile(Config& cfg, const std::string& profile) {
    unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    auto cap = [&](std::vector<unsigned> v) {
        std::vector<unsigned> out;
        for (auto t : v)
            if (t <= std::max(hw * 2, 4u)) out.push_back(t);
        return out;
    };
    cfg.profile = profile;
    if (profile == "quick") {
        cfg.file_size = 256ull << 20;
        cfg.duration_s = 2;
        cfg.block_sizes = {1 << 20};
        cfg.rand_block_sizes = {4096};
        cfg.threads = cap({1, 4});
        cfg.meta_files = 2000;
        cfg.tree_files = 1000;
    } else if (profile == "full") {
        cfg.file_size = 4ull << 30;
        cfg.duration_s = 10;
        cfg.block_sizes = {4096, 64 << 10, 1 << 20, 8 << 20};
        cfg.rand_block_sizes = {4096, 64 << 10};
        cfg.threads = cap({1, 2, 4, 8, 16, 32});
        cfg.meta_files = 50000;
        cfg.tree_files = 20000;
    } else if (profile == "standard") {
        cfg.file_size = 1ull << 30;
        cfg.duration_s = 5;
        cfg.block_sizes = {64 << 10, 1 << 20};
        cfg.rand_block_sizes = {4096};
        cfg.threads = cap({1, 4, 16});
        cfg.meta_files = 10000;
        cfg.tree_files = 5000;
    } else {
        throw std::invalid_argument("unknown profile '" + profile + "' (quick|standard|full)");
    }
    cfg.io_modes = {"buffered", "direct"};
}

// ---------------------------------------------------------------- results

const char* status_name(Status s) {
    switch (s) {
        case Status::Pass: return "PASS";
        case Status::Fail: return "FAIL";
        case Status::Unsupported: return "N/A";
        case Status::Skip: return "SKIP";
    }
    return "?";
}

void fail_check(const char* file, int line, const char* expr, const std::string& detail) {
    std::string f = file;
    if (auto pos = f.find_last_of("/\\"); pos != std::string::npos) f = f.substr(pos + 1);
    std::string msg = cat(f, ":", line, ": expected ", expr);
    if (!detail.empty()) msg += " -- " + detail;
    throw CheckFailure(msg);
}

// ---------------------------------------------------------------- contexts

Context::Context(const Config& c, const plat::SystemInfo& s, const TargetInfo& t, fs::path r,
                 std::optional<fs::path> alt, uint64_t seed)
    : cfg(c), sys(s), target(t), root(std::move(r)), alt_root(std::move(alt)), rng(seed) {}

BenchContext::BenchContext(const Config& c, const plat::SystemInfo& s, const TargetInfo& t, fs::path r,
                           std::optional<fs::path> alt, uint64_t seed, fs::path shared,
                           std::function<void(const BenchResult&)> sink)
    : Context(c, s, t, std::move(r), std::move(alt), seed), shared_dir(std::move(shared)), sink_(std::move(sink)) {}

void BenchContext::report(BenchResult r) {
    if (r.target.empty()) r.target = path_str(target.path);
    sink_(r);
}

fs::path BenchContext::shared_data_file(uint64_t size, const std::string& tag) {
    fs::path p = shared_dir / cat(tag, "_", size, ".bin");
    std::error_code ec;
    if (fs::exists(p, ec) && fs::file_size(p, ec) == size) return p;
    using namespace plat::open_flags;
    plat::File f = plat::File::open(p, write | create | truncate | sequential);
    const size_t chunk = 4 << 20;
    AlignedBuffer buf(chunk);
    Rng r(cfg.seed ^ size);
    for (uint64_t off = 0; off < size; off += chunk) {
        size_t n = static_cast<size_t>(std::min<uint64_t>(chunk, size - off));
        r.fill(buf.data(), n);
        f.pwrite(buf.data(), n, off);
    }
    f.sync();
    return p;
}

bool BenchContext::direct_io_supported() {
    static std::mutex mu;
    static std::map<std::string, bool> cache;
    std::lock_guard<std::mutex> lk(mu);
    auto key = path_str(shared_dir);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    bool ok = false;
    fs::path p = shared_dir / "direct_probe.bin";
    std::error_code ec;
    {
        using namespace plat::open_flags;
        plat::File f = plat::File::open(p, read | write | create | truncate | direct, ec);
        if (!ec) {
            AlignedBuffer buf(64 << 10);
            ok = capture_error([&] {
                     f.pwrite(buf.data(), buf.size(), 0);
                     f.pread(buf.data(), buf.size(), 0);
                 }) == std::error_code{};
        }
    }
    fs::remove(p, ec);
    cache[key] = ok;
    return ok;
}

// ---------------------------------------------------------------- registry

std::vector<TestDef>& test_registry() {
    static std::vector<TestDef> r;
    return r;
}

std::vector<BenchDef>& bench_registry() {
    static std::vector<BenchDef> r;
    return r;
}

TestRegistrar::TestRegistrar(const char* c, const char* n, const char* d, TestFn fn) {
    test_registry().push_back({c, n, d, fn});
}

BenchRegistrar::BenchRegistrar(const char* c, const char* n, const char* d, BenchFn fn) {
    bench_registry().push_back({c, n, d, fn});
}

bool selected(const Config& cfg, const std::string& id) {
    auto matches = [&](const std::string& pat) {
        // A bare word matches as a substring; patterns with wildcards must match fully.
        if (pat.find_first_of("*?") == std::string::npos) return id.find(pat) != std::string::npos;
        return glob_match(pat, id);
    };
    if (!cfg.filters.empty() && std::none_of(cfg.filters.begin(), cfg.filters.end(), matches)) return false;
    return std::none_of(cfg.excludes.begin(), cfg.excludes.end(), matches);
}

template <typename Def>
static std::vector<const Def*> ordered(const std::vector<Def>& defs, const std::vector<std::string>& order) {
    std::vector<const Def*> out;
    for (auto& d : defs) out.push_back(&d);
    auto rank = [&](const std::string& c) {
        auto it = std::find(order.begin(), order.end(), c);
        return static_cast<size_t>(it - order.begin());
    };
    std::stable_sort(out.begin(), out.end(), [&](const Def* a, const Def* b) { return rank(a->category) < rank(b->category); });
    return out;
}

// ---------------------------------------------------------------- runner

void force_remove_all(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
    if (!ec) return;
    // Make everything writable (read-only files/dirs block deletion on Windows,
    // read-only dirs block it on POSIX) and retry.
    fs::permissions(p, fs::perms::owner_all, fs::perm_options::add | fs::perm_options::nofollow, ec);
    for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (!it->is_symlink(e2))
            fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, e2);
    }
    fs::remove_all(p, ec);
    if (ec) throw fs::filesystem_error("remove_all", p, ec);
}

static std::optional<fs::path> make_alt(const std::optional<fs::path>& alt_work, const std::string& leaf) {
    if (!alt_work) return std::nullopt;
    fs::path p = *alt_work / leaf;
    fs::create_directories(p);
    return p;
}

static const std::vector<std::string> kTestOrder = {"io", "metadata", "dirs", "names", "ops",
                                                    "links", "features", "concurrency"};
static const std::vector<std::string> kBenchOrder = {"io", "latency", "meta", "ops"};

void run_tests(const Config& cfg, RunSummary& sum, const TargetInfo& target, const fs::path& work,
               const std::optional<fs::path>& alt_work, Console& con) {
    const std::string tname = path_str(target.path);
    for (const TestDef* def : ordered(test_registry(), kTestOrder)) {
        if (!selected(cfg, def->id())) continue;
        std::string leaf = def->category + "_" + def->name;
        fs::path root = work / "tests" / leaf;
        TestResult r;
        r.target = tname;
        r.category = def->category;
        r.name = def->name;
        r.description = def->description;
        con.progress("running " + def->id() + " ...");
        Timer t;
        std::vector<std::pair<std::string, std::string>> feats;
        try {
            fs::create_directories(root);
            TestContext ctx(cfg, sum.sys, target, root, make_alt(alt_work, leaf), cfg.seed ^ hash64(leaf.data(), leaf.size()));
            try {
                def->fn(ctx);
                r.status = Status::Pass;
            } catch (const SkipSignal& s) {
                r.status = s.unsupported ? Status::Unsupported : Status::Skip;
                r.message = s.reason;
            } catch (const CheckFailure& e) {
                r.status = Status::Fail;
                r.message = e.what();
            } catch (const std::exception& e) {
                r.status = Status::Fail;
                r.message = std::string("unexpected error: ") + e.what();
            }
            r.notes = ctx.notes;
            feats = ctx.features;
        } catch (const std::exception& e) {
            r.status = Status::Fail;
            r.message = std::string("setup error: ") + e.what();
        }
        r.seconds = t.elapsed_s();
        for (auto& [k, v] : feats) sum.features.push_back({tname, k, v});
        con.test_result(r);
        sum.tests.push_back(std::move(r));
        if (!cfg.keep) {
            try {
                force_remove_all(root);
                if (alt_work) force_remove_all(*alt_work / leaf);
            } catch (const std::exception& e) {
                con.warn("cleanup failed for " + def->id() + ": " + e.what());
            }
        }
    }
}

void run_benches(const Config& cfg, RunSummary& sum, const TargetInfo& target, const fs::path& work,
                 const std::optional<fs::path>& alt_work, Console& con) {
    const std::string tname = path_str(target.path);
    fs::path shared = work / "shared";
    fs::create_directories(shared);
    std::mutex mu;
    auto sink = [&](const BenchResult& r) {
        std::lock_guard<std::mutex> lk(mu);
        con.bench_result(r);
        sum.benches.push_back(r);
    };
    for (const BenchDef* def : ordered(bench_registry(), kBenchOrder)) {
        if (!selected(cfg, def->id())) continue;
        std::string leaf = def->category + "_" + def->name;
        fs::path root = work / "bench" / leaf;
        con.progress("running " + def->id() + " ...");
        try {
            fs::create_directories(root);
            BenchContext ctx(cfg, sum.sys, target, root, make_alt(alt_work, leaf),
                             cfg.seed ^ hash64(leaf.data(), leaf.size()), shared, sink);
            try {
                def->fn(ctx);
            } catch (const SkipSignal& s) {
                con.info(cat(def->id(), ": ", s.unsupported ? "not supported" : "skipped", " (", s.reason, ")"));
            }
            for (auto& [k, v] : ctx.features) sum.features.push_back({tname, k, v});
        } catch (const std::exception& e) {
            BenchResult r;
            r.target = tname;
            r.category = def->category;
            r.name = def->name;
            r.params = "-";
            r.failed = true;
            r.note = e.what();
            sink(r);
        }
        if (!cfg.keep) {
            try {
                force_remove_all(root);
                if (alt_work) force_remove_all(*alt_work / leaf);
            } catch (const std::exception& e) {
                con.warn("cleanup failed for " + def->id() + ": " + e.what());
            }
        }
    }
    if (!cfg.keep) {
        try {
            force_remove_all(shared);
        } catch (const std::exception& e) {
            con.warn(std::string("cleanup failed for shared data: ") + e.what());
        }
    }
}

}  // namespace fsb
