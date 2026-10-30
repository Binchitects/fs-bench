// Test / benchmark registry, execution contexts and assertion helpers.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "common.hpp"
#include "platform.hpp"

namespace fsb {

// ---------------------------------------------------------------- configuration

struct Config {
    std::vector<fs::path> targets;
    std::optional<fs::path> alt_target;  // second filesystem for cross-device move/copy
    std::string profile = "standard";
    std::vector<std::string> filters;    // glob patterns on "category/name"
    std::vector<std::string> excludes;

    uint64_t file_size = 1ull << 30;     // data file size for throughput benchmarks
    double duration_s = 5.0;             // time bound for random-I/O / latency benchmarks
    std::vector<size_t> block_sizes;     // sequential block sizes
    std::vector<size_t> rand_block_sizes;
    std::vector<unsigned> threads;
    std::vector<std::string> io_modes;   // "buffered", "direct"
    size_t meta_files = 10000;           // files for metadata benchmarks
    size_t tree_files = 5000;            // files for tree copy/move/delete benchmarks
    uint64_t seed = 0x5EEDF00Dull;

    bool keep = false;
    bool verbose = false;
    bool color = true;
    std::string json_out, csv_out, md_out;
};

void apply_profile(Config& cfg, const std::string& profile);

// ---------------------------------------------------------------- results

enum class Status { Pass, Fail, Unsupported, Skip };
const char* status_name(Status s);

struct TestResult {
    std::string target;
    std::string category, name, description;
    Status status = Status::Pass;
    std::string message;
    std::vector<std::string> notes;
    double seconds = 0;
    std::string id() const { return category + "/" + name; }
};

struct BenchResult {
    std::string target;
    std::string category, name, params;
    double seconds = 0;
    uint64_t bytes = 0;  // payload bytes moved (0 if not a throughput test)
    uint64_t ops = 0;    // operations completed
    bool has_latency = false;
    LatencySummary lat;
    std::string note;
    bool failed = false;  // benchmark aborted with an error (message in note)

    std::string id() const { return category + "/" + name; }
    double bytes_per_sec() const { return seconds > 0 ? static_cast<double>(bytes) / seconds : 0; }
    double ops_per_sec() const { return seconds > 0 ? static_cast<double>(ops) / seconds : 0; }
};

struct Feature {
    std::string target;
    std::string name;
    std::string value;
};

struct TargetInfo {
    fs::path path;
    plat::FsInfo fs;
};

struct RunSummary {
    plat::SystemInfo sys;
    std::vector<TargetInfo> targets;
    std::vector<TestResult> tests;
    std::vector<BenchResult> benches;
    std::vector<Feature> features;
    std::string started_at;
    double total_seconds = 0;
};

// ---------------------------------------------------------------- control-flow signals

struct SkipSignal {
    std::string reason;
    bool unsupported;
};

struct CheckFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail_check(const char* file, int line, const char* expr, const std::string& detail);

// ---------------------------------------------------------------- contexts

class Context {
public:
    Context(const Config& cfg, const plat::SystemInfo& sys, const TargetInfo& target, fs::path root,
            std::optional<fs::path> alt_root, uint64_t seed);

    const Config& cfg;
    const plat::SystemInfo& sys;
    const TargetInfo& target;
    const fs::path root;                 // fresh, empty scratch directory for this case
    const std::optional<fs::path> alt_root;  // scratch dir on the alternate filesystem
    Rng rng;

    fs::path path(std::string_view rel) const { return root / u8p(rel); }

    void note(std::string s) { notes.push_back(std::move(s)); }
    void feature(const std::string& name, const std::string& value) { features.push_back({name, value}); }
    [[noreturn]] void skip(std::string reason) { throw SkipSignal{std::move(reason), false}; }
    [[noreturn]] void unsupported(std::string reason) { throw SkipSignal{std::move(reason), true}; }

    std::vector<std::string> notes;
    std::vector<std::pair<std::string, std::string>> features;
};

class TestContext : public Context {
public:
    using Context::Context;
};

class BenchContext : public Context {
public:
    BenchContext(const Config& cfg, const plat::SystemInfo& sys, const TargetInfo& target, fs::path root,
                 std::optional<fs::path> alt_root, uint64_t seed, fs::path shared_dir,
                 std::function<void(const BenchResult&)> sink);

    void report(BenchResult r);
    // A data file of `size` incompressible bytes, created once and reused across benchmarks.
    // Benchmarks that modify the file use their own `tag` so read-only users see pristine data.
    fs::path shared_data_file(uint64_t size, const std::string& tag = "data");
    // True if direct I/O works on this filesystem (probed once).
    bool direct_io_supported();
    const fs::path shared_dir;

private:
    std::function<void(const BenchResult&)> sink_;
};

// ---------------------------------------------------------------- registry

using TestFn = void (*)(TestContext&);
using BenchFn = void (*)(BenchContext&);

struct TestDef {
    std::string category, name, description;
    TestFn fn;
    std::string id() const { return category + "/" + name; }
};

struct BenchDef {
    std::string category, name, description;
    BenchFn fn;
    std::string id() const { return category + "/" + name; }
};

std::vector<TestDef>& test_registry();
std::vector<BenchDef>& bench_registry();

struct TestRegistrar {
    TestRegistrar(const char* category, const char* name, const char* desc, TestFn fn);
};
struct BenchRegistrar {
    BenchRegistrar(const char* category, const char* name, const char* desc, BenchFn fn);
};

bool selected(const Config& cfg, const std::string& id);

// ---------------------------------------------------------------- runner

class Console;
void run_tests(const Config& cfg, RunSummary& sum, const TargetInfo& target, const fs::path& work,
               const std::optional<fs::path>& alt_work, Console& con);
void run_benches(const Config& cfg, RunSummary& sum, const TargetInfo& target, const fs::path& work,
                 const std::optional<fs::path>& alt_work, Console& con);

// Removes a tree even if it contains read-only entries.
void force_remove_all(const fs::path& p);

// ---------------------------------------------------------------- error helpers

template <typename F>
std::error_code capture_error(F&& f) {
    try {
        f();
        return {};
    } catch (const std::system_error& e) {
        return e.code();
    }
}

}  // namespace fsb

// ---------------------------------------------------------------- macros

#define FSB_CONCAT_(a, b) a##b
#define FSB_CONCAT(a, b) FSB_CONCAT_(a, b)

#define FSB_TEST(category, name, desc)                                                                \
    static void FSB_CONCAT(fsb_test_, FSB_CONCAT(category, FSB_CONCAT(_, name)))(::fsb::TestContext&); \
    static ::fsb::TestRegistrar FSB_CONCAT(fsb_treg_, FSB_CONCAT(category, FSB_CONCAT(_, name)))(       \
        #category, #name, desc, &FSB_CONCAT(fsb_test_, FSB_CONCAT(category, FSB_CONCAT(_, name))));     \
    static void FSB_CONCAT(fsb_test_, FSB_CONCAT(category, FSB_CONCAT(_, name)))(::fsb::TestContext & ctx)

#define FSB_BENCH(category, name, desc)                                                                 \
    static void FSB_CONCAT(fsb_bench_, FSB_CONCAT(category, FSB_CONCAT(_, name)))(::fsb::BenchContext&); \
    static ::fsb::BenchRegistrar FSB_CONCAT(fsb_breg_, FSB_CONCAT(category, FSB_CONCAT(_, name)))(        \
        #category, #name, desc, &FSB_CONCAT(fsb_bench_, FSB_CONCAT(category, FSB_CONCAT(_, name))));      \
    static void FSB_CONCAT(fsb_bench_, FSB_CONCAT(category, FSB_CONCAT(_, name)))(::fsb::BenchContext & ctx)

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) ::fsb::fail_check(__FILE__, __LINE__, #cond, std::string()); \
    } while (0)

#define CHECK_MSG(cond, ...)                                                          \
    do {                                                                              \
        if (!(cond)) ::fsb::fail_check(__FILE__, __LINE__, #cond, ::fsb::cat(__VA_ARGS__)); \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        const auto fsb_a_ = (a);                                                                 \
        const auto fsb_b_ = (b);                                                                 \
        if (!(fsb_a_ == fsb_b_))                                                                    \
            ::fsb::fail_check(__FILE__, __LINE__, #a " == " #b, ::fsb::cat("got ", fsb_a_, " vs ", fsb_b_)); \
    } while (0)

// Asserts that `expr` throws a system/filesystem error classified as `kind`.
#define CHECK_ERR(expr, kind)                                                                       \
    do {                                                                                            \
        auto fsb_ec_ = ::fsb::capture_error([&] { expr; });                                         \
        auto fsb_k_ = ::fsb::plat::classify(fsb_ec_);                                               \
        if (fsb_k_ != ::fsb::plat::ErrKind::kind)                                                   \
            ::fsb::fail_check(__FILE__, __LINE__, #expr " fails with " #kind,                       \
                              ::fsb::cat("got ", ::fsb::plat::err_kind_name(fsb_k_),                \
                                         fsb_ec_ ? " (" + fsb_ec_.message() + ")" : std::string())); \
    } while (0)

#define CHECK_THROWS(expr)                                                                      \
    do {                                                                                        \
        auto fsb_ec_ = ::fsb::capture_error([&] { expr; });                                     \
        if (!fsb_ec_) ::fsb::fail_check(__FILE__, __LINE__, #expr " fails", "it succeeded");    \
    } while (0)
