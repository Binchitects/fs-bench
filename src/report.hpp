// Console output and machine-readable reports (JSON, CSV, Markdown).
#pragma once

#include <mutex>
#include <string>

#include "framework.hpp"

namespace fsb {

class Console {
public:
    Console(bool color, bool verbose) : color_(color), verbose_(verbose) {}

    void section(const std::string& title);
    void line(const std::string& s);
    void info(const std::string& s);
    void warn(const std::string& s);
    void test_result(const TestResult& r);
    void bench_result(const BenchResult& r);
    void progress(const std::string& s);  // transient status line (cleared by next output)

    std::string green(const std::string& s) const { return paint("32", s); }
    std::string red(const std::string& s) const { return paint("31", s); }
    std::string yellow(const std::string& s) const { return paint("33", s); }
    std::string cyan(const std::string& s) const { return paint("36", s); }
    std::string dim(const std::string& s) const { return paint("2", s); }
    std::string bold(const std::string& s) const { return paint("1", s); }
    bool verbose() const { return verbose_; }

private:
    std::string paint(const char* code, const std::string& s) const;
    void clear_progress_locked();
    bool color_, verbose_;
    bool progress_shown_ = false;
    std::mutex mu_;
};

void print_system(Console& con, const plat::SystemInfo& sys);
void print_target(Console& con, const TargetInfo& t);
void print_summary(Console& con, const RunSummary& sum);

void write_json(const fs::path& out, const RunSummary& sum);
void write_csv(const fs::path& out, const RunSummary& sum);
void write_markdown(const fs::path& out, const RunSummary& sum);

std::string bench_value_str(const BenchResult& r);  // headline metric

}  // namespace fsb
