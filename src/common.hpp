// Shared utilities: timing, RNG, hashing, statistics, formatting, aligned buffers.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fsb {

namespace fs = std::filesystem;

// ---------------------------------------------------------------- timing

inline uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

struct Timer {
    uint64_t start = now_ns();
    void reset() { start = now_ns(); }
    uint64_t elapsed_ns() const { return now_ns() - start; }
    double elapsed_s() const { return static_cast<double>(elapsed_ns()) / 1e9; }
};

// ---------------------------------------------------------------- RNG (xoshiro256**)

class Rng {
public:
    explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ull);
    uint64_t next();
    uint64_t below(uint64_t n) { return n ? next() % n : 0; }
    double unit() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }
    // Fills buf with pseudo-random (incompressible) bytes.
    void fill(void* buf, size_t len);

private:
    uint64_t s_[4];
};

// ---------------------------------------------------------------- hashing

// Fast non-cryptographic 64-bit hash (wyhash-style mixing).
uint64_t hash64(const void* data, size_t len, uint64_t seed = 0);

// Streaming hash with a fixed internal chunking so that the result does not
// depend on how the caller splits the data.
class StreamHasher {
public:
    void update(const void* data, size_t len);
    uint64_t finish();

private:
    static constexpr size_t kChunk = 64 * 1024;
    std::vector<unsigned char> pending_;
    uint64_t state_ = 0x243F6A8885A308D3ull;
    uint64_t total_ = 0;
};

uint64_t hash_buffer(const void* data, size_t len);  // == StreamHasher over the whole buffer
uint64_t hash_file(const fs::path& p);               // throws on error
bool files_equal(const fs::path& a, const fs::path& b);

// ---------------------------------------------------------------- aligned buffer

class AlignedBuffer {
public:
    AlignedBuffer() = default;
    explicit AlignedBuffer(size_t size, size_t align = 4096);
    ~AlignedBuffer();
    AlignedBuffer(AlignedBuffer&& o) noexcept;
    AlignedBuffer& operator=(AlignedBuffer&& o) noexcept;
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;

    unsigned char* data() { return ptr_; }
    const unsigned char* data() const { return ptr_; }
    size_t size() const { return size_; }

private:
    unsigned char* ptr_ = nullptr;
    size_t size_ = 0;
};

// ---------------------------------------------------------------- statistics

struct LatencySummary {
    uint64_t count = 0;
    double min_ns = 0, max_ns = 0, mean_ns = 0, stddev_ns = 0;
    double p50_ns = 0, p90_ns = 0, p99_ns = 0, p999_ns = 0;
};

class LatencyRecorder {
public:
    void reserve(size_t n) { samples_.reserve(n); }
    void add(uint64_t ns) { samples_.push_back(ns); }
    void merge(const LatencyRecorder& o) { samples_.insert(samples_.end(), o.samples_.begin(), o.samples_.end()); }
    size_t size() const { return samples_.size(); }
    LatencySummary summarize();  // sorts in place

private:
    std::vector<uint64_t> samples_;
};

// ---------------------------------------------------------------- formatting / parsing

std::string fmt_bytes(double bytes);            // "1.50 GiB"
std::string fmt_rate(double bytes_per_sec);     // "512.3 MiB/s"
std::string fmt_count(double n);                // "12.3k"
std::string fmt_duration_ns(double ns);         // "12.4 us"
std::string fmt_fixed(double v, int prec);
uint64_t parse_size(std::string_view s);        // "4K", "1M", "2G", "4096"
double parse_duration_s(std::string_view s);    // "5", "5s", "500ms", "2m"
std::vector<std::string> split(std::string_view s, char sep);
std::string to_lower(std::string s);
bool glob_match(std::string_view pattern, std::string_view text);  // '*' and '?'
std::string path_str(const fs::path& p);        // UTF-8 representation
fs::path u8p(std::string_view utf8);            // path from UTF-8 text

template <typename... Args>
std::string cat(const Args&... args) {
    std::ostringstream os;
    (os << ... << args);
    return os.str();
}

// Writes `len` bytes of deterministic data derived from (seed, offset) into buf.
// Lets tests verify file content without keeping a copy in memory.
void pattern_fill(void* buf, size_t len, uint64_t seed, uint64_t offset);

}  // namespace fsb
