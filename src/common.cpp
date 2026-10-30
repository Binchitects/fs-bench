#include "common.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>

#ifdef _WIN32
#include <malloc.h>
#endif

namespace fsb {

// ---------------------------------------------------------------- RNG

static inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

static inline uint64_t splitmix64(uint64_t& x) {
    uint64_t z = (x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

Rng::Rng(uint64_t seed) {
    for (auto& s : s_) s = splitmix64(seed);
}

uint64_t Rng::next() {
    const uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
}

void Rng::fill(void* buf, size_t len) {
    auto* p = static_cast<unsigned char*>(buf);
    while (len >= 8) {
        uint64_t v = next();
        std::memcpy(p, &v, 8);
        p += 8;
        len -= 8;
    }
    if (len) {
        uint64_t v = next();
        std::memcpy(p, &v, len);
    }
}

// ---------------------------------------------------------------- hashing

static inline uint64_t mix64(uint64_t a, uint64_t b) {
#if defined(__SIZEOF_INT128__)
    __uint128_t r = static_cast<__uint128_t>(a) * b;
    return static_cast<uint64_t>(r) ^ static_cast<uint64_t>(r >> 64);
#else
    // Portable fallback (MSVC): 64x64->128 via 32-bit halves.
    uint64_t a_lo = a & 0xffffffff, a_hi = a >> 32, b_lo = b & 0xffffffff, b_hi = b >> 32;
    uint64_t p0 = a_lo * b_lo, p1 = a_lo * b_hi, p2 = a_hi * b_lo, p3 = a_hi * b_hi;
    uint64_t mid = (p0 >> 32) + (p1 & 0xffffffff) + (p2 & 0xffffffff);
    uint64_t lo = (p0 & 0xffffffff) | (mid << 32);
    uint64_t hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
    return lo ^ hi;
#endif
}

static constexpr uint64_t kP0 = 0xa0761d6478bd642full, kP1 = 0xe7037ed1a0b428dbull,
                          kP2 = 0x8ebc6af09c88c6e3ull, kP3 = 0x589965cc75374cc3ull;

uint64_t hash64(const void* data, size_t len, uint64_t seed) {
    const auto* p = static_cast<const unsigned char*>(data);
    uint64_t h = seed ^ kP0 ^ (len * kP1);
    size_t i = 0;
    for (; i + 32 <= len; i += 32) {
        uint64_t a, b, c, d;
        std::memcpy(&a, p + i, 8);
        std::memcpy(&b, p + i + 8, 8);
        std::memcpy(&c, p + i + 16, 8);
        std::memcpy(&d, p + i + 24, 8);
        h = mix64(a ^ kP1, b ^ h) ^ mix64(c ^ kP2, d ^ kP3);
    }
    for (; i + 8 <= len; i += 8) {
        uint64_t a;
        std::memcpy(&a, p + i, 8);
        h = mix64(a ^ kP1, h ^ kP2);
    }
    if (i < len) {
        uint64_t a = 0;
        std::memcpy(&a, p + i, len - i);
        h = mix64(a ^ kP3, h ^ kP1);
    }
    return mix64(h ^ kP0, len ^ kP2);
}

void StreamHasher::update(const void* data, size_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    total_ += len;
    if (!pending_.empty()) {
        size_t take = std::min(len, kChunk - pending_.size());
        pending_.insert(pending_.end(), p, p + take);
        p += take;
        len -= take;
        if (pending_.size() == kChunk) {
            state_ = hash64(pending_.data(), kChunk, state_);
            pending_.clear();
        }
    }
    while (len >= kChunk) {
        state_ = hash64(p, kChunk, state_);
        p += kChunk;
        len -= kChunk;
    }
    if (len) pending_.insert(pending_.end(), p, p + len);
}

uint64_t StreamHasher::finish() {
    if (!pending_.empty()) state_ = hash64(pending_.data(), pending_.size(), state_);
    pending_.clear();
    return mix64(state_ ^ kP3, total_ ^ kP0);
}

uint64_t hash_buffer(const void* data, size_t len) {
    StreamHasher h;
    h.update(data, len);
    return h.finish();
}

uint64_t hash_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open for hashing: " + path_str(p));
    std::vector<char> buf(1 << 20);
    StreamHasher h;
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        auto got = in.gcount();
        if (got > 0) h.update(buf.data(), static_cast<size_t>(got));
    }
    if (in.bad()) throw std::runtime_error("read error while hashing: " + path_str(p));
    return h.finish();
}

bool files_equal(const fs::path& a, const fs::path& b) {
    if (fs::file_size(a) != fs::file_size(b)) return false;
    std::ifstream ia(a, std::ios::binary), ib(b, std::ios::binary);
    if (!ia || !ib) return false;
    std::vector<char> ba(1 << 20), bb(1 << 20);
    while (ia && ib) {
        ia.read(ba.data(), static_cast<std::streamsize>(ba.size()));
        ib.read(bb.data(), static_cast<std::streamsize>(bb.size()));
        if (ia.gcount() != ib.gcount()) return false;
        if (std::memcmp(ba.data(), bb.data(), static_cast<size_t>(ia.gcount())) != 0) return false;
    }
    return !ia.bad() && !ib.bad();
}

void pattern_fill(void* buf, size_t len, uint64_t seed, uint64_t offset) {
    auto* out = static_cast<unsigned char*>(buf);
    while (len) {
        uint64_t word_idx = offset >> 3;
        unsigned in_word = static_cast<unsigned>(offset & 7);
        uint64_t s = seed ^ (word_idx * 0x9E3779B97F4A7C15ull);
        uint64_t v = splitmix64(s);
        size_t take = std::min<size_t>(len, 8 - in_word);
        std::memcpy(out, reinterpret_cast<unsigned char*>(&v) + in_word, take);
        out += take;
        len -= take;
        offset += take;
    }
}

// ---------------------------------------------------------------- aligned buffer

AlignedBuffer::AlignedBuffer(size_t size, size_t align) : size_(size) {
    size_t alloc = (size + align - 1) / align * align;
    if (alloc == 0) alloc = align;
#ifdef _WIN32
    ptr_ = static_cast<unsigned char*>(_aligned_malloc(alloc, align));
#else
    void* p = nullptr;
    if (posix_memalign(&p, align, alloc) != 0) p = nullptr;
    ptr_ = static_cast<unsigned char*>(p);
#endif
    if (!ptr_) throw std::bad_alloc();
    std::memset(ptr_, 0, alloc);
}

AlignedBuffer::~AlignedBuffer() {
#ifdef _WIN32
    _aligned_free(ptr_);
#else
    std::free(ptr_);
#endif
}

AlignedBuffer::AlignedBuffer(AlignedBuffer&& o) noexcept : ptr_(o.ptr_), size_(o.size_) {
    o.ptr_ = nullptr;
    o.size_ = 0;
}

AlignedBuffer& AlignedBuffer::operator=(AlignedBuffer&& o) noexcept {
    if (this != &o) {
        this->~AlignedBuffer();
        ptr_ = o.ptr_;
        size_ = o.size_;
        o.ptr_ = nullptr;
        o.size_ = 0;
    }
    return *this;
}

// ---------------------------------------------------------------- statistics

LatencySummary LatencyRecorder::summarize() {
    LatencySummary s;
    s.count = samples_.size();
    if (samples_.empty()) return s;
    std::sort(samples_.begin(), samples_.end());
    long double sum = 0;
    for (auto v : samples_) sum += v;
    s.mean_ns = static_cast<double>(sum / samples_.size());
    long double var = 0;
    for (auto v : samples_) {
        long double d = static_cast<long double>(v) - s.mean_ns;
        var += d * d;
    }
    s.stddev_ns = static_cast<double>(std::sqrt(var / samples_.size()));
    auto pct = [&](double q) {
        size_t idx = static_cast<size_t>(std::ceil(q * samples_.size())) - 1;
        if (idx >= samples_.size()) idx = samples_.size() - 1;
        return static_cast<double>(samples_[idx]);
    };
    s.min_ns = static_cast<double>(samples_.front());
    s.max_ns = static_cast<double>(samples_.back());
    s.p50_ns = pct(0.50);
    s.p90_ns = pct(0.90);
    s.p99_ns = pct(0.99);
    s.p999_ns = pct(0.999);
    return s;
}

// ---------------------------------------------------------------- formatting

std::string fmt_fixed(double v, int prec) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(prec) << v;
    return os.str();
}

std::string fmt_bytes(double b) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    int u = 0;
    while (std::fabs(b) >= 1024.0 && u < 5) {
        b /= 1024.0;
        ++u;
    }
    return fmt_fixed(b, u == 0 ? 0 : 2) + " " + units[u];
}

std::string fmt_rate(double bps) { return fmt_bytes(bps) + "/s"; }

std::string fmt_count(double n) {
    if (n >= 1e9) return fmt_fixed(n / 1e9, 2) + "G";
    if (n >= 1e6) return fmt_fixed(n / 1e6, 2) + "M";
    if (n >= 1e3) return fmt_fixed(n / 1e3, 1) + "k";
    return fmt_fixed(n, n == std::floor(n) ? 0 : 1);
}

std::string fmt_duration_ns(double ns) {
    if (ns >= 1e9) return fmt_fixed(ns / 1e9, 2) + " s";
    if (ns >= 1e6) return fmt_fixed(ns / 1e6, 2) + " ms";
    if (ns >= 1e3) return fmt_fixed(ns / 1e3, 1) + " us";
    return fmt_fixed(ns, 0) + " ns";
}

uint64_t parse_size(std::string_view s) {
    if (s.empty()) throw std::invalid_argument("empty size");
    size_t pos = 0;
    double v = std::stod(std::string(s), &pos);
    std::string suffix = to_lower(std::string(s.substr(pos)));
    if (!suffix.empty() && suffix.back() == 'b') suffix.pop_back();
    if (!suffix.empty() && suffix.back() == 'i') suffix.pop_back();
    double mult = 1;
    if (suffix.empty()) mult = 1;
    else if (suffix == "k") mult = 1024.0;
    else if (suffix == "m") mult = 1024.0 * 1024;
    else if (suffix == "g") mult = 1024.0 * 1024 * 1024;
    else if (suffix == "t") mult = 1024.0 * 1024 * 1024 * 1024;
    else throw std::invalid_argument("bad size suffix: " + std::string(s));
    return static_cast<uint64_t>(v * mult);
}

double parse_duration_s(std::string_view s) {
    size_t pos = 0;
    double v = std::stod(std::string(s), &pos);
    std::string suffix = to_lower(std::string(s.substr(pos)));
    if (suffix.empty() || suffix == "s") return v;
    if (suffix == "ms") return v / 1000.0;
    if (suffix == "m" || suffix == "min") return v * 60.0;
    throw std::invalid_argument("bad duration: " + std::string(s));
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(sep, start);
        if (end == std::string_view::npos) end = s.size();
        auto part = s.substr(start, end - start);
        if (!part.empty()) out.emplace_back(part);
        start = end + 1;
    }
    return out;
}

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool glob_match(std::string_view pat, std::string_view txt) {
    size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
    while (t < txt.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == txt[t])) {
            ++p;
            ++t;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

std::string path_str(const fs::path& p) {
    auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

fs::path u8p(std::string_view utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

}  // namespace fsb
