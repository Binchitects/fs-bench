// fsbench - filesystem functional test suite and benchmark.
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#define fsb_isatty(f) _isatty(_fileno(f))
#define fsb_getpid() _getpid()
#else
#include <unistd.h>
#define fsb_isatty(f) isatty(fileno(f))
#define fsb_getpid() getpid()
#endif

#include "framework.hpp"
#include "report.hpp"

using namespace fsb;

static const char* kUsage = R"(fsbench - filesystem functional tests and performance benchmarks

Usage: fsbench [command] [options]

Commands:
  all        run functional tests, then benchmarks (default)
  test       run functional tests only
  bench      run benchmarks only
  list       list all tests and benchmarks
  info       show system and filesystem information for the targets

Options:
  -t, --target DIR         directory on the filesystem under test (repeat to compare
                           several filesystems side by side; default: current dir)
  -a, --alt-target DIR     directory on a *different* filesystem, enables cross-device
                           move/copy tests and benchmarks
  -p, --profile NAME       quick | standard | full            (default: standard)
  -f, --filter LIST        only run cases matching these comma-separated patterns,
                           matched against "category/name"; '*' and '?' wildcards,
                           plain words match as substrings   (e.g. "io/*,copy")
  -x, --exclude LIST       skip cases matching these patterns
      --file-size SIZE     data size for throughput benchmarks  (e.g. 512M, 4G)
      --duration TIME      time per random-I/O / latency run   (e.g. 5s, 500ms)
      --block-sizes LIST   sequential block sizes              (e.g. 4K,64K,1M)
      --rand-block-sizes L random I/O block sizes              (e.g. 4K,16K)
      --threads LIST       thread counts for scaling runs      (e.g. 1,4,16)
      --io-modes LIST      buffered,direct                     (default: both)
      --files N            file count for metadata benchmarks
      --tree-files N       file count for tree copy/move/delete benchmarks
      --seed N             RNG seed
      --json FILE          write full results as JSON
      --csv FILE           write benchmark results as CSV (+ FILE_tests.csv)
      --md FILE            write a Markdown report
      --keep               keep scratch files after the run
      --no-color           disable ANSI colors
  -v, --verbose            more output
  -h, --help               show this help

Profiles:
  quick     256M data, 2s runs, 1M blocks, threads 1,4, 2k files     (~1-2 min)
  standard  1G data, 5s runs, 64K/1M blocks, threads 1,4,16, 10k files
  full      4G data, 10s runs, 4K..8M blocks, threads 1..32, 50k files

Examples:
  fsbench test -t /mnt/data
  fsbench bench -t /mnt/ext4 -t /mnt/xfs -t /mnt/btrfs --json cmp.json
  fsbench all -t D:\bench -a E:\bench --profile quick --md report.md
  fsbench bench -t /mnt/nvme -f "io/rand*" --threads 1,8,32 --duration 10s
)";

static std::string now_iso() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

template <typename T, typename F>
static std::vector<T> parse_list(const std::string& s, F conv) {
    std::vector<T> out;
    for (auto& part : split(s, ',')) out.push_back(static_cast<T>(conv(part)));
    if (out.empty()) throw std::invalid_argument("empty list: " + s);
    return out;
}

int main(int argc, char** argv) {
    plat::platform_init();
    Config cfg;
    std::string command = "all";
    std::vector<std::string> args(argv + 1, argv + argc);

    try {
        // Pass 1: command and profile (so explicit options can override profile defaults).
        std::string profile = "standard";
        if (!args.empty() && args[0].rfind("-", 0) != 0) {
            command = args[0];
            args.erase(args.begin());
        }
        for (size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == "-p" || args[i] == "--profile") profile = args[i + 1];
        apply_profile(cfg, profile);
        cfg.color = fsb_isatty(stdout) != 0;

        // Pass 2: everything else.
        for (size_t i = 0; i < args.size(); ++i) {
            const std::string& a = args[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= args.size()) throw std::invalid_argument("missing value for " + a);
                return args[++i];
            };
            if (a == "-h" || a == "--help") {
                std::cout << kUsage;
                return 0;
            } else if (a == "-t" || a == "--target") cfg.targets.push_back(u8p(next()));
            else if (a == "-a" || a == "--alt-target") cfg.alt_target = u8p(next());
            else if (a == "-p" || a == "--profile") next();
            else if (a == "-f" || a == "--filter") {
                auto v = split(next(), ',');
                cfg.filters.insert(cfg.filters.end(), v.begin(), v.end());
            } else if (a == "-x" || a == "--exclude") {
                auto v = split(next(), ',');
                cfg.excludes.insert(cfg.excludes.end(), v.begin(), v.end());
            } else if (a == "--file-size") cfg.file_size = parse_size(next());
            else if (a == "--duration") cfg.duration_s = parse_duration_s(next());
            else if (a == "--block-sizes") cfg.block_sizes = parse_list<size_t>(next(), parse_size);
            else if (a == "--rand-block-sizes") cfg.rand_block_sizes = parse_list<size_t>(next(), parse_size);
            else if (a == "--threads") cfg.threads = parse_list<unsigned>(next(), [](const std::string& s) { return std::stoul(s); });
            else if (a == "--io-modes") {
                cfg.io_modes = split(next(), ',');
                for (auto& m : cfg.io_modes)
                    if (m != "buffered" && m != "direct") throw std::invalid_argument("io mode must be buffered or direct");
            } else if (a == "--files") cfg.meta_files = std::stoul(next());
            else if (a == "--tree-files") cfg.tree_files = std::stoul(next());
            else if (a == "--seed") cfg.seed = std::stoull(next());
            else if (a == "--json") cfg.json_out = next();
            else if (a == "--csv") cfg.csv_out = next();
            else if (a == "--md") cfg.md_out = next();
            else if (a == "--keep") cfg.keep = true;
            else if (a == "--no-color") cfg.color = false;
            else if (a == "-v" || a == "--verbose") cfg.verbose = true;
            else throw std::invalid_argument("unknown option: " + a);
        }
        if (command != "all" && command != "test" && command != "bench" && command != "list" && command != "info")
            throw std::invalid_argument("unknown command: " + command);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n\nRun 'fsbench --help' for usage.\n";
        return 2;
    }

    Console con(cfg.color, cfg.verbose);

    if (command == "list") {
        std::string last;
        con.section(cat("Functional tests (", test_registry().size(), ")"));
        for (auto& t : test_registry())
            if (selected(cfg, t.id())) con.line("  " + con.bold(t.id()) + std::string(t.id().size() < 36 ? 36 - t.id().size() : 1, ' ') + t.description);
        con.section(cat("Benchmarks (", bench_registry().size(), ")"));
        for (auto& b : bench_registry())
            if (selected(cfg, b.id())) con.line("  " + con.bold(b.id()) + std::string(b.id().size() < 36 ? 36 - b.id().size() : 1, ' ') + b.description);
        return 0;
    }

    if (cfg.targets.empty()) cfg.targets.push_back(fs::current_path());

    RunSummary sum;
    sum.started_at = now_iso();
    sum.sys = plat::system_info();
    Timer total;
    print_system(con, sum.sys);

    const std::string run_tag = cat("fsbench-", fsb_getpid(), "-", std::time(nullptr));
    std::optional<fs::path> alt_work;
    if (cfg.alt_target && command != "info") {
        try {
            alt_work = fs::absolute(*cfg.alt_target) / run_tag;
            fs::create_directories(*alt_work);
        } catch (const std::exception& e) {
            std::cerr << "error: cannot use alt target: " << e.what() << "\n";
            return 2;
        }
    }

    for (auto& tpath : cfg.targets) {
        TargetInfo ti;
        try {
            ti.path = fs::absolute(tpath);
            if (!fs::is_directory(ti.path)) throw std::runtime_error("not a directory: " + path_str(ti.path));
            ti.fs = plat::fs_info(ti.path);
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            return 2;
        }
        sum.targets.push_back(ti);
    }
    // TargetInfo references must stay stable from here on.
    for (auto& ti : sum.targets) {
        con.section("Target " + path_str(ti.path));
        print_target(con, ti);
        if (cfg.alt_target) {
            auto alt_fs = plat::fs_info(*cfg.alt_target);
            con.line("  Alt target : " + path_str(*cfg.alt_target) + " (" + alt_fs.type + " on " + alt_fs.device + ")");
        }
        if (command == "info") continue;

        fs::path work = ti.path / run_tag;
        try {
            fs::create_directories(work);
        } catch (const std::exception& e) {
            con.warn(std::string("cannot create work directory: ") + e.what());
            continue;
        }

        if (command == "all" || command == "test") {
            con.section("Functional tests on " + ti.fs.type + " (" + path_str(ti.path) + ")");
            run_tests(cfg, sum, ti, work, alt_work, con);
        }
        if (command == "all" || command == "bench") {
            Config bcfg = cfg;
            uint64_t avail = plat::fs_info(work).avail_bytes;
            uint64_t need = bcfg.file_size * 3 + (512ull << 20);
            if (avail && avail < need) {
                uint64_t fit = (avail / 4) / (64ull << 20) * (64ull << 20);
                con.warn(cat("only ", fmt_bytes(static_cast<double>(avail)), " free; reducing --file-size from ",
                             fmt_bytes(static_cast<double>(bcfg.file_size)), " to ", fmt_bytes(static_cast<double>(fit))));
                bcfg.file_size = std::max<uint64_t>(fit, 64ull << 20);
            }
            if (bcfg.file_size < sum.sys.ram_bytes && std::find(bcfg.io_modes.begin(), bcfg.io_modes.end(), "buffered") != bcfg.io_modes.end())
                con.info(cat("note: data size (", fmt_bytes(static_cast<double>(bcfg.file_size)),
                             ") is smaller than RAM; buffered reads may partly hit the page cache "
                             "(the suite drops caches where the OS allows; 'direct' mode bypasses it)"));
            con.section("Benchmarks on " + ti.fs.type + " (" + path_str(ti.path) + ")");
            run_benches(bcfg, sum, ti, work, alt_work, con);
        }
        if (!cfg.keep) {
            try {
                force_remove_all(work);
            } catch (const std::exception& e) {
                con.warn(std::string("could not remove work dir: ") + e.what());
            }
        }
    }
    if (alt_work && !cfg.keep) {
        try {
            force_remove_all(*alt_work);
        } catch (...) {
        }
    }

    sum.total_seconds = total.elapsed_s();
    if (command != "info") print_summary(con, sum);

    try {
        if (!cfg.json_out.empty()) write_json(u8p(cfg.json_out), sum), con.info("wrote " + cfg.json_out);
        if (!cfg.csv_out.empty()) write_csv(u8p(cfg.csv_out), sum), con.info("wrote " + cfg.csv_out);
        if (!cfg.md_out.empty()) write_markdown(u8p(cfg.md_out), sum), con.info("wrote " + cfg.md_out);
    } catch (const std::exception& e) {
        con.warn(e.what());
    }

    bool failed = std::any_of(sum.tests.begin(), sum.tests.end(), [](auto& r) { return r.status == Status::Fail; }) ||
                  std::any_of(sum.benches.begin(), sum.benches.end(), [](auto& b) { return b.failed; });
    return failed ? 1 : 0;
}
