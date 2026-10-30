#include "report.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <set>

namespace fsb {

// ---------------------------------------------------------------- helpers

static size_t display_width(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

static std::string pad(const std::string& s, size_t w) {
    size_t d = display_width(s);
    return d >= w ? s : s + std::string(w - d, ' ');
}

static std::string lpad(const std::string& s, size_t w) {
    size_t d = display_width(s);
    return d >= w ? s : std::string(w - d, ' ') + s;
}

using Row = std::vector<std::string>;

static void print_table(Console& con, const Row& header, const std::vector<Row>& rows, size_t right_from = 1) {
    std::vector<size_t> w(header.size(), 0);
    for (size_t i = 0; i < header.size(); ++i) w[i] = display_width(header[i]);
    for (auto& r : rows)
        for (size_t i = 0; i < r.size() && i < w.size(); ++i) w[i] = std::max(w[i], display_width(r[i]));
    auto fmt_row = [&](const Row& r) {
        std::string line = "  ";
        for (size_t i = 0; i < w.size(); ++i) {
            std::string cell = i < r.size() ? r[i] : "";
            line += (i >= right_from ? lpad(cell, w[i]) : pad(cell, w[i]));
            if (i + 1 < w.size()) line += "  ";
        }
        return line;
    };
    con.line(con.bold(fmt_row(header)));
    size_t total = 0;
    for (auto x : w) total += x + 2;
    con.line("  " + std::string(total > 2 ? total - 2 : 0, '-'));
    for (auto& r : rows) con.line(fmt_row(r));
}

std::string bench_value_str(const BenchResult& r) {
    if (r.failed) return "ERROR";
    if (r.bytes > 0) return fmt_rate(r.bytes_per_sec());
    if (r.ops == 1) return fmt_duration_ns(r.seconds * 1e9);  // single operation: report its duration
    return fmt_count(r.ops_per_sec()) + " ops/s";
}

// ---------------------------------------------------------------- Console

std::string Console::paint(const char* code, const std::string& s) const {
    if (!color_) return s;
    return std::string("\x1b[") + code + "m" + s + "\x1b[0m";
}

void Console::clear_progress_locked() {
    if (progress_shown_) {
        std::cout << "\r\x1b[K";
        progress_shown_ = false;
    }
}

void Console::progress(const std::string& s) {
    if (!color_) return;  // only on interactive terminals
    std::lock_guard<std::mutex> lk(mu_);
    clear_progress_locked();
    std::cout << dim("  " + s) << std::flush;
    progress_shown_ = true;
}

void Console::section(const std::string& title) {
    std::lock_guard<std::mutex> lk(mu_);
    clear_progress_locked();
    std::cout << "\n" << bold(cyan("== " + title + " ==")) << "\n";
}

void Console::line(const std::string& s) {
    std::lock_guard<std::mutex> lk(mu_);
    clear_progress_locked();
    std::cout << s << "\n";
}

void Console::info(const std::string& s) { line("  " + dim(s)); }

void Console::warn(const std::string& s) { line("  " + yellow("warning: ") + s); }

void Console::test_result(const TestResult& r) {
    std::string tag;
    switch (r.status) {
        case Status::Pass: tag = green("[PASS]"); break;
        case Status::Fail: tag = red("[FAIL]"); break;
        case Status::Unsupported: tag = yellow("[N/A] "); break;
        case Status::Skip: tag = dim("[SKIP]"); break;
    }
    std::string out = "  " + tag + " " + pad(r.id(), 44) + " " + dim(lpad(fmt_duration_ns(r.seconds * 1e9), 9));
    if (!r.message.empty()) {
        std::string msg = r.status == Status::Fail ? red(r.message) : dim(r.message);
        out += "\n         " + msg;
    }
    for (auto& n : r.notes) out += "\n         " + dim("- " + n);
    line(out);
}

void Console::bench_result(const BenchResult& r) {
    if (r.failed) {
        line("  " + red("[ERR] ") + pad(r.id(), 26) + " " + red(r.note));
        return;
    }
    std::string out = "  " + pad(r.id(), 26) + " " + pad(r.params, 34) + " " + bold(lpad(bench_value_str(r), 14));
    if (r.bytes > 0 && r.ops > 1) out += "  " + lpad(fmt_count(r.ops_per_sec()) + " ops/s", 14);
    else if (r.bytes > 0) out += "  " + lpad("in " + fmt_duration_ns(r.seconds * 1e9), 14);
    if (r.has_latency)
        out += dim(cat("  lat avg ", fmt_duration_ns(r.lat.mean_ns), " p50 ", fmt_duration_ns(r.lat.p50_ns), " p99 ",
                       fmt_duration_ns(r.lat.p99_ns), " p99.9 ", fmt_duration_ns(r.lat.p999_ns)));
    if (!r.note.empty()) out += dim("  [" + r.note + "]");
    line(out);
}

// ---------------------------------------------------------------- summaries

void print_system(Console& con, const plat::SystemInfo& sys) {
    con.section("System");
    con.line("  OS       : " + sys.os);
    con.line("  Kernel   : " + sys.kernel);
    con.line("  CPU      : " + sys.cpu + cat(" (", sys.cpu_count, " logical)"));
    con.line("  RAM      : " + fmt_bytes(static_cast<double>(sys.ram_bytes)));
    con.line(std::string("  Privilege: ") + (sys.privileged ? "root/administrator" : "regular user"));
}

void print_target(Console& con, const TargetInfo& t) {
    const auto& f = t.fs;
    con.line("  Path       : " + path_str(t.path));
    con.line("  Filesystem : " + con.bold(f.type) + (f.volume_label.empty() ? "" : "  label=" + f.volume_label));
    con.line("  Device     : " + f.device + "  (mounted at " + f.mount_point + ")");
    con.line(cat("  Capacity   : ", fmt_bytes(static_cast<double>(f.total_bytes)), " total, ",
                 fmt_bytes(static_cast<double>(f.avail_bytes)), " available, block size ", f.block_size));
    if (f.total_inodes) con.line(cat("  Inodes     : ", f.total_inodes, " total, ", f.free_inodes, " free"));
    con.line(cat("  Max name   : ", f.max_name_len));
    if (!f.mount_options.empty()) con.line("  Options    : " + f.mount_options);
    if (!f.features.empty()) {
        std::string s;
        for (auto& x : f.features) s += (s.empty() ? "" : " ") + x;
        if (s != f.mount_options) con.line("  Features   : " + s);
    }
}

void print_summary(Console& con, const RunSummary& sum) {
    std::vector<std::string> targets;
    for (auto& t : sum.targets) targets.push_back(path_str(t.path));

    if (!sum.tests.empty()) {
        con.section("Test summary");
        std::vector<Row> rows;
        for (auto& t : targets) {
            std::map<Status, int> c;
            for (auto& r : sum.tests)
                if (r.target == t) c[r.status]++;
            rows.push_back({t, std::to_string(c[Status::Pass]), std::to_string(c[Status::Fail]),
                            std::to_string(c[Status::Unsupported]), std::to_string(c[Status::Skip])});
        }
        print_table(con, {"target", "pass", "fail", "n/a", "skip"}, rows);
        bool any_fail = false;
        for (auto& r : sum.tests) {
            if (r.status != Status::Fail) continue;
            if (!any_fail) con.line("\n  " + con.red("Failures:"));
            any_fail = true;
            con.line("  - " + (targets.size() > 1 ? "[" + r.target + "] " : "") + r.id() + ": " + r.message);
        }
    }

    if (!sum.features.empty()) {
        con.section("Filesystem feature matrix");
        std::vector<std::string> names;
        std::map<std::pair<std::string, std::string>, std::string> val;
        for (auto& f : sum.features) {
            if (std::find(names.begin(), names.end(), f.name) == names.end()) names.push_back(f.name);
            val[{f.name, f.target}] = f.value;
        }
        Row header = {"feature"};
        for (auto& t : sum.targets) header.push_back(t.fs.type.empty() ? path_str(t.path) : t.fs.type);
        std::vector<Row> rows;
        for (auto& n : names) {
            Row r = {n};
            for (auto& t : targets) {
                auto it = val.find({n, t});
                r.push_back(it == val.end() ? "-" : it->second);
            }
            rows.push_back(r);
        }
        print_table(con, header, rows, 99);
    }

    if (!sum.benches.empty()) {
        con.section(targets.size() > 1 ? "Benchmark comparison" : "Benchmark summary");
        std::vector<std::pair<std::string, std::string>> keys;
        std::map<std::pair<std::string, std::string>, std::map<std::string, const BenchResult*>> by;
        for (auto& b : sum.benches) {
            auto k = std::make_pair(b.id(), b.params);
            if (!by.count(k)) keys.push_back(k);
            by[k][b.target] = &b;
        }
        Row header = {"benchmark", "params"};
        for (auto& t : sum.targets) header.push_back(t.fs.type.empty() ? path_str(t.path) : t.fs.type);
        if (targets.size() == 1) header.push_back("p99 latency");
        std::vector<Row> rows;
        for (auto& k : keys) {
            Row r = {k.first, k.second};
            double best = 0;
            for (auto& t : targets) {
                auto it = by[k].find(t);
                if (it == by[k].end() || it->second->failed) continue;
                const auto* b = it->second;
                best = std::max(best, b->bytes ? b->bytes_per_sec() : b->ops == 1 ? 1.0 / b->seconds : b->ops_per_sec());
            }
            for (auto& t : targets) {
                auto it = by[k].find(t);
                if (it == by[k].end()) {
                    r.push_back("-");
                    continue;
                }
                const auto* b = it->second;
                std::string v = bench_value_str(*b);
                double mine = b->bytes ? b->bytes_per_sec() : b->ops == 1 ? 1.0 / b->seconds : b->ops_per_sec();
                if (targets.size() > 1 && !b->failed && best > 0)
                    v += cat(" (", fmt_fixed(100.0 * mine / best, 0), "%)");
                r.push_back(v);
            }
            if (targets.size() == 1) {
                const auto* b = by[k].begin()->second;
                r.push_back(b->has_latency ? fmt_duration_ns(b->lat.p99_ns) : "");
            }
            rows.push_back(r);
        }
        print_table(con, header, rows, 2);
    }
    con.line(cat("\n  Total time: ", fmt_fixed(sum.total_seconds, 1), " s"));
}

// ---------------------------------------------------------------- JSON

namespace {

std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o + "\"";
}

std::string jnum(double v) {
    if (!(v == v) || v > 1e300 || v < -1e300) return "null";
    std::ostringstream os;
    os.precision(10);
    os << v;
    return os.str();
}

std::string jarr(const std::vector<std::string>& v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i) o += (i ? "," : "") + jstr(v[i]);
    return o + "]";
}

}  // namespace

void write_json(const fs::path& out, const RunSummary& sum) {
    std::ofstream o(out, std::ios::binary);
    if (!o) throw std::runtime_error("cannot write " + path_str(out));
    o << "{\n  \"tool\": \"fsbench\",\n  \"version\": \"1.0\",\n";
    o << "  \"platform\": " << jstr(plat::platform_name()) << ",\n";
    o << "  \"started_at\": " << jstr(sum.started_at) << ",\n";
    o << "  \"total_seconds\": " << jnum(sum.total_seconds) << ",\n";
    const auto& s = sum.sys;
    o << "  \"system\": {\"os\": " << jstr(s.os) << ", \"kernel\": " << jstr(s.kernel) << ", \"cpu\": " << jstr(s.cpu)
      << ", \"cpu_count\": " << s.cpu_count << ", \"ram_bytes\": " << s.ram_bytes
      << ", \"hostname\": " << jstr(s.hostname) << ", \"privileged\": " << (s.privileged ? "true" : "false") << "},\n";

    o << "  \"targets\": [";
    for (size_t i = 0; i < sum.targets.size(); ++i) {
        const auto& t = sum.targets[i];
        const auto& f = t.fs;
        o << (i ? "," : "") << "\n    {\"path\": " << jstr(path_str(t.path)) << ", \"fs_type\": " << jstr(f.type)
          << ", \"device\": " << jstr(f.device) << ", \"mount_point\": " << jstr(f.mount_point)
          << ", \"mount_options\": " << jstr(f.mount_options) << ", \"label\": " << jstr(f.volume_label)
          << ", \"block_size\": " << f.block_size << ", \"total_bytes\": " << f.total_bytes
          << ", \"avail_bytes\": " << f.avail_bytes << ", \"max_name_len\": " << f.max_name_len
          << ", \"features\": " << jarr(f.features) << "}";
    }
    o << "\n  ],\n";

    o << "  \"tests\": [";
    for (size_t i = 0; i < sum.tests.size(); ++i) {
        const auto& r = sum.tests[i];
        o << (i ? "," : "") << "\n    {\"target\": " << jstr(r.target) << ", \"id\": " << jstr(r.id())
          << ", \"category\": " << jstr(r.category) << ", \"name\": " << jstr(r.name)
          << ", \"description\": " << jstr(r.description) << ", \"status\": " << jstr(status_name(r.status))
          << ", \"message\": " << jstr(r.message) << ", \"seconds\": " << jnum(r.seconds)
          << ", \"notes\": " << jarr(r.notes) << "}";
    }
    o << "\n  ],\n";

    o << "  \"features\": [";
    for (size_t i = 0; i < sum.features.size(); ++i) {
        const auto& f = sum.features[i];
        o << (i ? "," : "") << "\n    {\"target\": " << jstr(f.target) << ", \"name\": " << jstr(f.name)
          << ", \"value\": " << jstr(f.value) << "}";
    }
    o << "\n  ],\n";

    o << "  \"benchmarks\": [";
    for (size_t i = 0; i < sum.benches.size(); ++i) {
        const auto& b = sum.benches[i];
        o << (i ? "," : "") << "\n    {\"target\": " << jstr(b.target) << ", \"id\": " << jstr(b.id())
          << ", \"category\": " << jstr(b.category) << ", \"name\": " << jstr(b.name)
          << ", \"params\": " << jstr(b.params) << ", \"failed\": " << (b.failed ? "true" : "false")
          << ", \"seconds\": " << jnum(b.seconds) << ", \"bytes\": " << b.bytes << ", \"ops\": " << b.ops
          << ", \"bytes_per_sec\": " << jnum(b.bytes_per_sec()) << ", \"ops_per_sec\": " << jnum(b.ops_per_sec());
        if (b.has_latency) {
            const auto& l = b.lat;
            o << ", \"latency_ns\": {\"count\": " << l.count << ", \"min\": " << jnum(l.min_ns)
              << ", \"mean\": " << jnum(l.mean_ns) << ", \"stddev\": " << jnum(l.stddev_ns)
              << ", \"p50\": " << jnum(l.p50_ns) << ", \"p90\": " << jnum(l.p90_ns) << ", \"p99\": " << jnum(l.p99_ns)
              << ", \"p999\": " << jnum(l.p999_ns) << ", \"max\": " << jnum(l.max_ns) << "}";
        }
        o << ", \"note\": " << jstr(b.note) << "}";
    }
    o << "\n  ]\n}\n";
}

// ---------------------------------------------------------------- CSV

static std::string csv(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) o += c == '"' ? std::string("\"\"") : std::string(1, c);
    return o + "\"";
}

void write_csv(const fs::path& out, const RunSummary& sum) {
    {
        std::ofstream o(out, std::ios::binary);
        if (!o) throw std::runtime_error("cannot write " + path_str(out));
        o << "target,category,name,params,seconds,bytes,ops,mib_per_sec,ops_per_sec,"
             "lat_mean_us,lat_p50_us,lat_p90_us,lat_p99_us,lat_p999_us,lat_max_us,failed,note\n";
        for (auto& b : sum.benches) {
            o << csv(b.target) << "," << b.category << "," << b.name << "," << csv(b.params) << ","
              << jnum(b.seconds) << "," << b.bytes << "," << b.ops << "," << jnum(b.bytes_per_sec() / 1048576.0)
              << "," << jnum(b.ops_per_sec()) << ",";
            if (b.has_latency) {
                const auto& l = b.lat;
                o << jnum(l.mean_ns / 1e3) << "," << jnum(l.p50_ns / 1e3) << "," << jnum(l.p90_ns / 1e3) << ","
                  << jnum(l.p99_ns / 1e3) << "," << jnum(l.p999_ns / 1e3) << "," << jnum(l.max_ns / 1e3);
            } else {
                o << ",,,,,";
            }
            o << "," << (b.failed ? 1 : 0) << "," << csv(b.note) << "\n";
        }
    }
    fs::path tests_out = out;
    tests_out.replace_filename(out.stem().string() + "_tests" + out.extension().string());
    std::ofstream o(tests_out, std::ios::binary);
    o << "target,category,name,status,seconds,message,notes\n";
    for (auto& r : sum.tests) {
        std::string notes;
        for (auto& n : r.notes) notes += (notes.empty() ? "" : "; ") + n;
        o << csv(r.target) << "," << r.category << "," << r.name << "," << status_name(r.status) << ","
          << jnum(r.seconds) << "," << csv(r.message) << "," << csv(notes) << "\n";
    }
}

// ---------------------------------------------------------------- Markdown

static std::string md(const std::string& s) {
    std::string o;
    for (char c : s) o += c == '|' ? std::string("\\|") : std::string(1, c);
    return o;
}

void write_markdown(const fs::path& out, const RunSummary& sum) {
    std::ofstream o(out, std::ios::binary);
    if (!o) throw std::runtime_error("cannot write " + path_str(out));
    const auto& s = sum.sys;
    o << "# Filesystem test & benchmark report\n\n";
    o << "- **Started:** " << sum.started_at << "  \n- **OS:** " << md(s.os) << " (" << md(s.kernel) << ")  \n"
      << "- **CPU:** " << md(s.cpu) << " (" << s.cpu_count << " logical)  \n- **RAM:** "
      << fmt_bytes(static_cast<double>(s.ram_bytes)) << "\n\n";

    o << "## Targets\n\n| Path | Filesystem | Device | Block | Capacity | Options |\n|---|---|---|---|---|---|\n";
    for (auto& t : sum.targets)
        o << "| " << md(path_str(t.path)) << " | " << md(t.fs.type) << " | " << md(t.fs.device) << " | "
          << t.fs.block_size << " | " << fmt_bytes(static_cast<double>(t.fs.total_bytes)) << " | "
          << md(t.fs.mount_options) << " |\n";

    if (!sum.tests.empty()) {
        o << "\n## Functional tests\n\n| Target | Test | Status | Details |\n|---|---|---|---|\n";
        for (auto& r : sum.tests) {
            std::string details = r.message;
            for (auto& n : r.notes) details += (details.empty() ? "" : "<br>") + n;
            o << "| " << md(r.target) << " | `" << r.id() << "` | " << status_name(r.status) << " | " << md(details)
              << " |\n";
        }
    }

    if (!sum.features.empty()) {
        o << "\n## Feature matrix\n\n| Feature |";
        for (auto& t : sum.targets) o << " " << md(t.fs.type) << " |";
        o << "\n|---|";
        for (size_t i = 0; i < sum.targets.size(); ++i) o << "---|";
        o << "\n";
        std::vector<std::string> names;
        std::map<std::pair<std::string, std::string>, std::string> val;
        for (auto& f : sum.features) {
            if (std::find(names.begin(), names.end(), f.name) == names.end()) names.push_back(f.name);
            val[{f.name, f.target}] = f.value;
        }
        for (auto& n : names) {
            o << "| " << md(n) << " |";
            for (auto& t : sum.targets) {
                auto it = val.find({n, path_str(t.path)});
                o << " " << (it == val.end() ? "-" : md(it->second)) << " |";
            }
            o << "\n";
        }
    }

    if (!sum.benches.empty()) {
        o << "\n## Benchmarks\n\n| Target | Benchmark | Params | Throughput | Ops/s | Lat avg | p50 | p99 | p99.9 | Note "
             "|\n|---|---|---|---|---|---|---|---|---|---|\n";
        for (auto& b : sum.benches) {
            o << "| " << md(b.target) << " | `" << b.id() << "` | " << md(b.params) << " | "
              << (b.failed ? "ERROR" : b.bytes ? fmt_rate(b.bytes_per_sec()) : "") << " | "
              << (b.failed ? "" : fmt_count(b.ops_per_sec())) << " | ";
            if (b.has_latency)
                o << fmt_duration_ns(b.lat.mean_ns) << " | " << fmt_duration_ns(b.lat.p50_ns) << " | "
                  << fmt_duration_ns(b.lat.p99_ns) << " | " << fmt_duration_ns(b.lat.p999_ns);
            else
                o << " |  |  | ";
            o << " | " << md(b.note) << " |\n";
        }
    }
}

}  // namespace fsb
