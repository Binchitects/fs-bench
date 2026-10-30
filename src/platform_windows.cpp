// Windows implementation of the platform layer (Win32 API, Windows 10+).
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "common.hpp"
#include "platform.hpp"

#ifndef FSCTL_DUPLICATE_EXTENTS_TO_FILE
#define FSCTL_DUPLICATE_EXTENTS_TO_FILE CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 209, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

namespace fsb::plat {

namespace {

const HANDLE kInvalid = INVALID_HANDLE_VALUE;
constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr int64_t kEpochDiff100ns = 116444736000000000LL;  // 1601-01-01 -> 1970-01-01
constexpr DWORD kSupportsBlockRefcounting = 0x08000000;    // FILE_SUPPORTS_BLOCK_REFCOUNTING
constexpr DWORD kNamedStreams = 0x00040000;                // FILE_NAMED_STREAMS

std::error_code win_ec(DWORD e = GetLastError()) { return {static_cast<int>(e), std::system_category()}; }

[[noreturn]] void throw_win(const std::string& what, const fs::path& p, DWORD e = GetLastError()) {
    throw std::system_error(win_ec(e), what + " '" + path_str(p) + "'");
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Absolute path with the \\?\ prefix so that paths longer than MAX_PATH work
// regardless of the system's LongPathsEnabled policy.
std::wstring long_path(const fs::path& p) {
    fs::path abs = p.is_absolute() ? p : fs::absolute(p);
    abs = abs.lexically_normal();
    abs.make_preferred();
    std::wstring s = abs.wstring();
    if (s.rfind(L"\\\\?\\", 0) == 0) return s;
    if (s.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + s.substr(2);
    return L"\\\\?\\" + s;
}

int64_t ft_to_ns(int64_t ft100) { return (ft100 - kEpochDiff100ns) * 100; }

int64_t ns_to_ft(int64_t ns) {
    int64_t q = ns / 100;
    if (ns % 100 < 0) --q;  // floor division
    return q + kEpochDiff100ns;
}

FileStat stat_handle(HANDLE h, const fs::path& p) {
    FILE_BASIC_INFO bi{};
    FILE_STANDARD_INFO si{};
    BY_HANDLE_FILE_INFORMATION bh{};
    if (!GetFileInformationByHandleEx(h, FileBasicInfo, &bi, sizeof(bi))) throw_win("GetFileInformationByHandleEx(Basic)", p);
    if (!GetFileInformationByHandleEx(h, FileStandardInfo, &si, sizeof(si))) throw_win("GetFileInformationByHandleEx(Standard)", p);
    if (!GetFileInformationByHandle(h, &bh)) throw_win("GetFileInformationByHandle", p);
    FileStat st;
    st.size = static_cast<uint64_t>(si.EndOfFile.QuadPart);
    st.allocated = static_cast<uint64_t>(si.AllocationSize.QuadPart);
    if (bi.FileAttributes & (FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_COMPRESSED)) {
        FILE_COMPRESSION_INFO ci{};
        if (GetFileInformationByHandleEx(h, FileCompressionInfo, &ci, sizeof(ci)))
            st.allocated = static_cast<uint64_t>(ci.CompressedFileSize.QuadPart);
    }
    st.nlink = si.NumberOfLinks;
    st.device = bh.dwVolumeSerialNumber;
    st.inode = (static_cast<uint64_t>(bh.nFileIndexHigh) << 32) | bh.nFileIndexLow;
    st.atime_ns = ft_to_ns(bi.LastAccessTime.QuadPart);
    st.mtime_ns = ft_to_ns(bi.LastWriteTime.QuadPart);
    st.ctime_ns = ft_to_ns(bi.ChangeTime.QuadPart);
    st.btime_ns = ft_to_ns(bi.CreationTime.QuadPart);
    st.attributes = bi.FileAttributes;
    st.is_dir = (bi.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    st.is_symlink = (bi.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    st.is_regular = !st.is_dir && !st.is_symlink;
    st.mode = st.is_dir ? 0755 : ((bi.FileAttributes & FILE_ATTRIBUTE_READONLY) ? 0444 : 0644);
    return st;
}

std::wstring volume_root(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec) abs = p;
    // Use the nearest existing ancestor so that the lookup also works for paths about to be created.
    while (!abs.empty() && !fs::exists(abs, ec) && abs.has_parent_path() && abs.parent_path() != abs)
        abs = abs.parent_path();
    wchar_t buf[MAX_PATH + 1] = {};
    if (!GetVolumePathNameW(abs.wstring().c_str(), buf, MAX_PATH + 1)) return abs.root_path().wstring();
    return buf;
}

DWORD volume_flags(const fs::path& p) {
    DWORD flags = 0, maxcomp = 0, serial = 0;
    GetVolumeInformationW(volume_root(p).c_str(), nullptr, 0, &serial, &maxcomp, &flags, nullptr, 0);
    return flags;
}

uint64_t cluster_size(const fs::path& p) {
    DWORD spc = 0, bps = 0, fc = 0, tc = 0;
    if (!GetDiskFreeSpaceW(volume_root(p).c_str(), &spc, &bps, &fc, &tc)) return 4096;
    return static_cast<uint64_t>(spc) * bps;
}

struct HandleGuard {
    HANDLE h;
    ~HandleGuard() {
        if (h && h != kInvalid) CloseHandle(h);
    }
};

}  // namespace

// ---------------------------------------------------------------- MappedRegion

MappedRegion::~MappedRegion() { unmap(); }

MappedRegion::MappedRegion(MappedRegion&& o) noexcept
    : addr_(std::exchange(o.addr_, nullptr)),
      len_(std::exchange(o.len_, 0)),
      mapping_(std::exchange(o.mapping_, nullptr)),
      file_(std::exchange(o.file_, nullptr)) {}

MappedRegion& MappedRegion::operator=(MappedRegion&& o) noexcept {
    if (this != &o) {
        unmap();
        addr_ = std::exchange(o.addr_, nullptr);
        len_ = std::exchange(o.len_, 0);
        mapping_ = std::exchange(o.mapping_, nullptr);
        file_ = std::exchange(o.file_, nullptr);
    }
    return *this;
}

void MappedRegion::flush() {
    if (!addr_) return;
    if (!FlushViewOfFile(addr_, len_)) throw std::system_error(win_ec(), "FlushViewOfFile");
    if (file_ && !FlushFileBuffers(static_cast<HANDLE>(file_))) throw std::system_error(win_ec(), "FlushFileBuffers");
}

void MappedRegion::unmap() {
    if (addr_) UnmapViewOfFile(addr_);
    if (mapping_) CloseHandle(static_cast<HANDLE>(mapping_));
    addr_ = nullptr;
    mapping_ = nullptr;
    file_ = nullptr;
    len_ = 0;
}

// ---------------------------------------------------------------- File

File::~File() { close(); }

File::File(File&& o) noexcept : h_(std::exchange(o.h_, kInvalid)), path_(std::move(o.path_)), flags_(o.flags_) {}

File& File::operator=(File&& o) noexcept {
    if (this != &o) {
        close();
        h_ = std::exchange(o.h_, kInvalid);
        path_ = std::move(o.path_);
        flags_ = o.flags_;
    }
    return *this;
}

bool File::valid() const { return h_ != kInvalid && h_ != nullptr; }

void File::close() {
    if (valid()) CloseHandle(h_);
    h_ = kInvalid;
}

File File::open(const fs::path& p, unsigned flags) {
    std::error_code ec;
    File f = open(p, flags, ec);
    if (ec) throw std::system_error(ec, "open '" + path_str(p) + "'");
    return f;
}

File File::open(const fs::path& p, unsigned flags, std::error_code& ec) {
    namespace of = open_flags;
    DWORD access = 0;
    if (flags & of::read) access |= GENERIC_READ;
    if (flags & of::append) {
        // FILE_APPEND_DATA without FILE_WRITE_DATA: every write lands atomically at EOF.
        access |= FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | STANDARD_RIGHTS_WRITE | SYNCHRONIZE;
    } else if (flags & of::write) {
        access |= GENERIC_WRITE;
    }
    if (access == 0) access = GENERIC_READ;

    DWORD disp;
    if ((flags & of::create) && (flags & of::exclusive)) disp = CREATE_NEW;
    else if ((flags & of::create) && (flags & of::truncate)) disp = CREATE_ALWAYS;
    else if (flags & of::create) disp = OPEN_ALWAYS;
    else if (flags & of::truncate) disp = TRUNCATE_EXISTING;
    else disp = OPEN_EXISTING;

    DWORD attrs = FILE_ATTRIBUTE_NORMAL;
    if (flags & of::direct) attrs |= FILE_FLAG_NO_BUFFERING;
    if (flags & (of::sync | of::dsync)) attrs |= FILE_FLAG_WRITE_THROUGH;
    if (flags & of::sequential) attrs |= FILE_FLAG_SEQUENTIAL_SCAN;
    if (flags & of::random) attrs |= FILE_FLAG_RANDOM_ACCESS;

    File f;
    HANDLE h = CreateFileW(long_path(p).c_str(), access, kShareAll, nullptr, disp, attrs, nullptr);
    if (h == kInvalid) {
        ec = win_ec();
        return f;
    }
    f.h_ = h;
    f.path_ = p;
    f.flags_ = flags;
    ec.clear();
    return f;
}

File File::open_dir(const fs::path& p, std::error_code& ec) {
    File f;
    HANDLE h = CreateFileW(long_path(p).c_str(), GENERIC_READ | GENERIC_WRITE, kShareAll, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == kInvalid)
        h = CreateFileW(long_path(p).c_str(), GENERIC_READ, kShareAll, nullptr, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == kInvalid) {
        ec = win_ec();
        return f;
    }
    f.h_ = h;
    f.path_ = p;
    ec.clear();
    return f;
}

File File::open_temp_anonymous(const fs::path& dir, std::error_code& ec) {
    // Windows has no unnamed files; the closest equivalent is a delete-on-close temporary.
    File f;
    fs::path p = dir / cat(".fsb-anon-", GetCurrentProcessId(), "-", GetTickCount64());
    HANDLE h = CreateFileW(long_path(p).c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, kShareAll, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == kInvalid) {
        ec = win_ec();
        return f;
    }
    f.h_ = h;
    f.path_ = p;
    f.flags_ = open_flags::read | open_flags::write;
    ec.clear();
    return f;
}

size_t File::pread(void* buf, size_t len, uint64_t off) {
    auto* p = static_cast<char*>(buf);
    size_t done = 0;
    while (done < len) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(len - done, 1u << 30));
        OVERLAPPED ov{};
        uint64_t o = off + done;
        ov.Offset = static_cast<DWORD>(o);
        ov.OffsetHigh = static_cast<DWORD>(o >> 32);
        DWORD got = 0;
        if (!ReadFile(h_, p + done, chunk, &got, &ov)) {
            DWORD e = GetLastError();
            if (e == ERROR_HANDLE_EOF) break;
            throw_win("ReadFile", path_, e);
        }
        if (got == 0) break;
        done += got;
    }
    return done;
}

size_t File::pwrite(const void* buf, size_t len, uint64_t off) {
    const auto* p = static_cast<const char*>(buf);
    size_t done = 0;
    while (done < len) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(len - done, 1u << 30));
        OVERLAPPED ov{};
        uint64_t o = off + done;
        ov.Offset = static_cast<DWORD>(o);
        ov.OffsetHigh = static_cast<DWORD>(o >> 32);
        DWORD put = 0;
        if (!WriteFile(h_, p + done, chunk, &put, &ov)) throw_win("WriteFile", path_);
        done += put;
    }
    return done;
}

size_t File::read(void* buf, size_t len) {
    auto* p = static_cast<char*>(buf);
    size_t done = 0;
    while (done < len) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(len - done, 1u << 30));
        DWORD got = 0;
        if (!ReadFile(h_, p + done, chunk, &got, nullptr)) throw_win("ReadFile", path_);
        if (got == 0) break;
        done += got;
    }
    return done;
}

size_t File::write(const void* buf, size_t len) {
    const auto* p = static_cast<const char*>(buf);
    size_t done = 0;
    while (done < len) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(len - done, 1u << 30));
        DWORD put = 0;
        if (!WriteFile(h_, p + done, chunk, &put, nullptr)) throw_win("WriteFile", path_);
        done += put;
    }
    return done;
}

uint64_t File::seek(int64_t off, int whence) {
    LARGE_INTEGER li, out;
    li.QuadPart = off;
    DWORD method = whence == 0 ? FILE_BEGIN : whence == 1 ? FILE_CURRENT : FILE_END;
    if (!SetFilePointerEx(h_, li, &out, method)) throw_win("SetFilePointerEx", path_);
    return static_cast<uint64_t>(out.QuadPart);
}

uint64_t File::size() {
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h_, &li)) throw_win("GetFileSizeEx", path_);
    return static_cast<uint64_t>(li.QuadPart);
}

void File::truncate(uint64_t n) {
    FILE_END_OF_FILE_INFO eof{};
    eof.EndOfFile.QuadPart = static_cast<LONGLONG>(n);
    if (!SetFileInformationByHandle(h_, FileEndOfFileInfo, &eof, sizeof(eof))) throw_win("SetEndOfFile", path_);
}

void File::sync() {
    if (!FlushFileBuffers(h_)) throw_win("FlushFileBuffers", path_);
}

void File::datasync() { sync(); }

FileStat File::stat() { return stat_handle(h_, path_); }

void File::preallocate(uint64_t off, uint64_t len, std::error_code& ec) {
    FILE_ALLOCATION_INFO ai{};
    ai.AllocationSize.QuadPart = static_cast<LONGLONG>(off + len);
    if (!SetFileInformationByHandle(h_, FileAllocationInfo, &ai, sizeof(ai))) ec = win_ec();
    else ec.clear();
}

void File::set_sparse(std::error_code& ec) {
    DWORD br = 0;
    if (!DeviceIoControl(h_, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &br, nullptr)) ec = win_ec();
    else ec.clear();
}

void File::punch_hole(uint64_t off, uint64_t len, std::error_code& ec) {
    FILE_ZERO_DATA_INFORMATION zi{};
    zi.FileOffset.QuadPart = static_cast<LONGLONG>(off);
    zi.BeyondFinalZero.QuadPart = static_cast<LONGLONG>(off + len);
    DWORD br = 0;
    if (!DeviceIoControl(h_, FSCTL_SET_ZERO_DATA, &zi, sizeof(zi), nullptr, 0, &br, nullptr)) ec = win_ec();
    else ec.clear();
}

std::vector<Extent> File::data_ranges(std::error_code& ec) {
    std::vector<Extent> out;
    ec.clear();
    FILE_ALLOCATED_RANGE_BUFFER in{};
    in.FileOffset.QuadPart = 0;
    in.Length.QuadPart = static_cast<LONGLONG>(size());
    FILE_ALLOCATED_RANGE_BUFFER ranges[64];
    for (;;) {
        DWORD br = 0;
        BOOL ok = DeviceIoControl(h_, FSCTL_QUERY_ALLOCATED_RANGES, &in, sizeof(in), ranges, sizeof(ranges), &br, nullptr);
        DWORD e = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && e != ERROR_MORE_DATA) {
            ec = win_ec(e);
            return {};
        }
        size_t n = br / sizeof(FILE_ALLOCATED_RANGE_BUFFER);
        for (size_t i = 0; i < n; ++i)
            out.push_back({static_cast<uint64_t>(ranges[i].FileOffset.QuadPart),
                           static_cast<uint64_t>(ranges[i].Length.QuadPart)});
        if (ok || n == 0) break;
        LONGLONG end = ranges[n - 1].FileOffset.QuadPart + ranges[n - 1].Length.QuadPart;
        in.Length.QuadPart -= end - in.FileOffset.QuadPart;
        in.FileOffset.QuadPart = end;
    }
    return out;
}

bool File::try_lock(uint64_t off, uint64_t len, bool exclusive) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(off);
    ov.OffsetHigh = static_cast<DWORD>(off >> 32);
    DWORD fl = LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0);
    if (LockFileEx(h_, fl, 0, static_cast<DWORD>(len), static_cast<DWORD>(len >> 32), &ov)) return true;
    DWORD e = GetLastError();
    if (e == ERROR_LOCK_VIOLATION || e == ERROR_IO_PENDING) return false;
    throw_win("LockFileEx", path_, e);
}

void File::unlock(uint64_t off, uint64_t len) {
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(off);
    ov.OffsetHigh = static_cast<DWORD>(off >> 32);
    if (!UnlockFileEx(h_, 0, static_cast<DWORD>(len), static_cast<DWORD>(len >> 32), &ov))
        throw_win("UnlockFileEx", path_);
}

MappedRegion File::map(uint64_t off, size_t len, bool writable) {
    uint64_t end = off + len;
    HANDLE m = CreateFileMappingW(h_, nullptr, writable ? PAGE_READWRITE : PAGE_READONLY, static_cast<DWORD>(end >> 32),
                                  static_cast<DWORD>(end), nullptr);
    if (!m) throw_win("CreateFileMapping", path_);
    void* a = MapViewOfFile(m, writable ? FILE_MAP_WRITE : FILE_MAP_READ, static_cast<DWORD>(off >> 32),
                            static_cast<DWORD>(off), len);
    if (!a) {
        DWORD e = GetLastError();
        CloseHandle(m);
        throw_win("MapViewOfFile", path_, e);
    }
    MappedRegion r;
    r.addr_ = a;
    r.len_ = len;
    r.mapping_ = m;
    r.file_ = h_;
    return r;
}

void File::link_to(const fs::path&, std::error_code& ec) { ec = win_ec(ERROR_NOT_SUPPORTED); }

bool File::drop_cache() {
    FlushFileBuffers(h_);
    return drop_file_cache(path_);
}

// ---------------------------------------------------------------- path operations

FileStat stat_path(const fs::path& p, bool follow) {
    DWORD fl = FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT);
    HandleGuard g{CreateFileW(long_path(p).c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING, fl, nullptr)};
    if (g.h == kInvalid) throw_win("stat", p);
    return stat_handle(g.h, p);
}

void set_file_times(const fs::path& p, int64_t atime_ns, int64_t mtime_ns) {
    HandleGuard g{CreateFileW(long_path(p).c_str(), FILE_WRITE_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (g.h == kInvalid) throw_win("open for set_file_times", p);
    FILE_BASIC_INFO bi{};  // zero = leave unchanged
    bi.LastAccessTime.QuadPart = ns_to_ft(atime_ns);
    bi.LastWriteTime.QuadPart = ns_to_ft(mtime_ns);
    if (!SetFileInformationByHandle(g.h, FileBasicInfo, &bi, sizeof(bi))) throw_win("SetFileInformationByHandle(Basic)", p);
}

// Rename with POSIX semantics (Windows 10 1607+ on NTFS): replaces the target even
// while other handles have it open. Returns false if not available.
static bool rename_posix(const fs::path& from, const fs::path& to) {
    struct RenameInfoEx {  // layout of FILE_RENAME_INFO with the Flags member
        DWORD Flags;
        HANDLE RootDirectory;
        DWORD FileNameLength;
        WCHAR FileName[1];
    };
    constexpr DWORD kReplaceIfExists = 0x1, kPosixSemantics = 0x2;
    constexpr int kFileRenameInfoEx = 22;
    HandleGuard g{CreateFileW(long_path(from).c_str(), DELETE | SYNCHRONIZE, kShareAll, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (g.h == kInvalid) return false;
    std::wstring target = long_path(to);
    std::vector<unsigned char> buf(sizeof(RenameInfoEx) + target.size() * sizeof(WCHAR));
    auto* ri = reinterpret_cast<RenameInfoEx*>(buf.data());
    ri->Flags = kReplaceIfExists | kPosixSemantics;
    ri->RootDirectory = nullptr;
    ri->FileNameLength = static_cast<DWORD>(target.size() * sizeof(WCHAR));
    std::memcpy(ri->FileName, target.data(), target.size() * sizeof(WCHAR));
    return SetFileInformationByHandle(g.h, static_cast<FILE_INFO_BY_HANDLE_CLASS>(kFileRenameInfoEx), ri,
                                      static_cast<DWORD>(buf.size())) != 0;
}

void rename_replace(const fs::path& from, const fs::path& to) {
    if (rename_posix(from, to)) return;
    if (!MoveFileExW(long_path(from).c_str(), long_path(to).c_str(), MOVEFILE_REPLACE_EXISTING))
        throw_win("MoveFileEx", from);
}

void rename_noreplace(const fs::path& from, const fs::path& to, std::error_code& ec) {
    if (!MoveFileExW(long_path(from).c_str(), long_path(to).c_str(), 0)) ec = win_ec();
    else ec.clear();
}

void rename_exchange(const fs::path&, const fs::path&, std::error_code& ec) { ec = win_ec(ERROR_NOT_SUPPORTED); }

void copy_kernel(const fs::path& src, const fs::path& dst) {
    if (!CopyFileExW(long_path(src).c_str(), long_path(dst).c_str(), nullptr, nullptr, nullptr, 0))
        throw_win("CopyFileEx", src);
}

void copy_reflink(const fs::path& src, const fs::path& dst, std::error_code& ec) {
    // Block cloning (ReFS / Dev Drive) via FSCTL_DUPLICATE_EXTENTS_TO_FILE.
    if (!(volume_flags(dst.parent_path()) & kSupportsBlockRefcounting)) {
        ec = win_ec(ERROR_NOT_SUPPORTED);
        return;
    }
    struct DuplicateExtents {
        HANDLE FileHandle;
        LARGE_INTEGER SourceFileOffset;
        LARGE_INTEGER TargetFileOffset;
        LARGE_INTEGER ByteCount;
    };
    HandleGuard in{CreateFileW(long_path(src).c_str(), GENERIC_READ, kShareAll, nullptr, OPEN_EXISTING, 0, nullptr)};
    if (in.h == kInvalid) {
        ec = win_ec();
        return;
    }
    HandleGuard out{CreateFileW(long_path(dst).c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, kShareAll, nullptr,
                                CREATE_ALWAYS, 0, nullptr)};
    if (out.h == kInvalid) {
        ec = win_ec();
        return;
    }
    auto fail = [&](DWORD e) {
        ec = win_ec(e);
        CloseHandle(out.h);
        out.h = kInvalid;
        DeleteFileW(long_path(dst).c_str());
    };
    LARGE_INTEGER size;
    GetFileSizeEx(in.h, &size);
    FILE_BASIC_INFO bi{};
    GetFileInformationByHandleEx(in.h, FileBasicInfo, &bi, sizeof(bi));
    DWORD br = 0;
    if (bi.FileAttributes & FILE_ATTRIBUTE_SPARSE_FILE)
        DeviceIoControl(out.h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &br, nullptr);
    FILE_END_OF_FILE_INFO eof{};
    eof.EndOfFile = size;
    if (!SetFileInformationByHandle(out.h, FileEndOfFileInfo, &eof, sizeof(eof))) return fail(GetLastError());
    const uint64_t cluster = cluster_size(dst.parent_path());
    const uint64_t chunk = 1ull << 30;
    for (uint64_t off = 0; off < static_cast<uint64_t>(size.QuadPart); off += chunk) {
        uint64_t len = std::min<uint64_t>(chunk, static_cast<uint64_t>(size.QuadPart) - off);
        len = (len + cluster - 1) / cluster * cluster;
        DuplicateExtents d{};
        d.FileHandle = in.h;
        d.SourceFileOffset.QuadPart = static_cast<LONGLONG>(off);
        d.TargetFileOffset.QuadPart = static_cast<LONGLONG>(off);
        d.ByteCount.QuadPart = static_cast<LONGLONG>(len);
        if (!DeviceIoControl(out.h, FSCTL_DUPLICATE_EXTENTS_TO_FILE, &d, sizeof(d), nullptr, 0, &br, nullptr))
            return fail(GetLastError());
    }
    ec.clear();
}

// ---------------------------------------------------------------- alternate data streams

static std::wstring stream_path(const fs::path& p, const std::string& name) { return long_path(p) + L":" + widen(name); }

void xattr_set(const fs::path& p, const std::string& name, const std::string& value, std::error_code& ec) {
    if (!(volume_flags(p) & kNamedStreams)) {
        ec = win_ec(ERROR_NOT_SUPPORTED);
        return;
    }
    HandleGuard g{CreateFileW(stream_path(p, name).c_str(), GENERIC_WRITE, kShareAll, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (g.h == kInvalid) {
        ec = win_ec();
        return;
    }
    DWORD put = 0;
    if (!value.empty() && !WriteFile(g.h, value.data(), static_cast<DWORD>(value.size()), &put, nullptr)) {
        ec = win_ec();
        return;
    }
    ec.clear();
}

std::string xattr_get(const fs::path& p, const std::string& name, std::error_code& ec) {
    HandleGuard g{CreateFileW(stream_path(p, name).c_str(), GENERIC_READ, kShareAll, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (g.h == kInvalid) {
        ec = win_ec();
        return {};
    }
    LARGE_INTEGER sz;
    GetFileSizeEx(g.h, &sz);
    std::string v(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD got = 0;
    if (!v.empty() && !ReadFile(g.h, v.data(), static_cast<DWORD>(v.size()), &got, nullptr)) {
        ec = win_ec();
        return {};
    }
    v.resize(got);
    ec.clear();
    return v;
}

std::vector<std::string> xattr_list(const fs::path& p, std::error_code& ec) {
    std::vector<std::string> out;
    WIN32_FIND_STREAM_DATA d{};
    HANDLE h = FindFirstStreamW(long_path(p).c_str(), FindStreamInfoStandard, &d, 0);
    if (h == kInvalid) {
        DWORD e = GetLastError();
        if (e == ERROR_HANDLE_EOF) {
            ec.clear();
            return out;
        }
        ec = win_ec(e);
        return out;
    }
    do {
        std::wstring n = d.cStreamName;  // ":name:$DATA"
        if (n.size() > 1 && n[0] == L':') {
            auto end = n.find(L':', 1);
            std::wstring stream = n.substr(1, end == std::wstring::npos ? std::wstring::npos : end - 1);
            if (!stream.empty()) out.push_back(narrow(stream));
        }
    } while (FindNextStreamW(h, &d));
    FindClose(h);
    ec.clear();
    return out;
}

void xattr_remove(const fs::path& p, const std::string& name, std::error_code& ec) {
    if (!DeleteFileW(stream_path(p, name).c_str())) ec = win_ec();
    else ec.clear();
}

std::string xattr_backend() { return "alternate data streams (NTFS/ReFS)"; }

bool drop_file_cache(const fs::path& p) {
    // Opening a non-cached handle makes the cache manager flush and purge the
    // file's cached pages (best effort; there is no unprivileged per-file purge).
    HandleGuard g{CreateFileW(long_path(p).c_str(), GENERIC_READ, kShareAll, nullptr, OPEN_EXISTING,
                              FILE_FLAG_NO_BUFFERING, nullptr)};
    return g.h != kInvalid;
}

bool sync_filesystem(const fs::path& p) {
    std::wstring root = volume_root(p);  // "C:\"
    if (root.size() < 2 || root[1] != L':') return false;
    std::wstring dev = L"\\\\.\\" + root.substr(0, 2);
    HandleGuard g{CreateFileW(dev.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, 0, nullptr)};
    if (g.h == kInvalid) return false;  // requires elevation
    return FlushFileBuffers(g.h) != 0;
}

// ---------------------------------------------------------------- DirWatcher

struct DirWatcher::Impl {
    HANDLE dir = INVALID_HANDLE_VALUE;
    HANDLE ev = nullptr;
    OVERLAPPED ov{};
    alignas(DWORD) unsigned char buf[64 * 1024];
    bool pending = false;

    bool issue() {
        ResetEvent(ev);
        ov = OVERLAPPED{};
        ov.hEvent = ev;
        DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                       FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_ATTRIBUTES;
        pending = ReadDirectoryChangesW(dir, buf, sizeof(buf), FALSE, filter, nullptr, &ov, nullptr) != 0;
        return pending;
    }
};

DirWatcher::DirWatcher() : impl_(std::make_unique<Impl>()) {}

DirWatcher::~DirWatcher() {
    if (impl_->pending) {
        DWORD b = 0;
        CancelIoEx(impl_->dir, &impl_->ov);
        GetOverlappedResult(impl_->dir, &impl_->ov, &b, TRUE);
    }
    if (impl_->dir != kInvalid) CloseHandle(impl_->dir);
    if (impl_->ev) CloseHandle(impl_->ev);
}

void DirWatcher::start(const fs::path& dir, std::error_code& ec) {
    impl_->dir = CreateFileW(long_path(dir).c_str(), FILE_LIST_DIRECTORY, kShareAll, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (impl_->dir == kInvalid) {
        ec = win_ec();
        return;
    }
    impl_->ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->issue()) {
        ec = win_ec();
        return;
    }
    ec.clear();
}

std::vector<std::string> DirWatcher::wait(int timeout_ms) {
    std::vector<std::string> names;
    if (!impl_->pending) return names;
    if (WaitForSingleObject(impl_->ev, static_cast<DWORD>(timeout_ms)) != WAIT_OBJECT_0) return names;
    DWORD bytes = 0;
    BOOL ok = GetOverlappedResult(impl_->dir, &impl_->ov, &bytes, FALSE);
    impl_->pending = false;
    if (ok && bytes == 0) names.emplace_back("");  // overflow: events happened but were dropped
    for (DWORD off = 0; ok && bytes > 0;) {
        auto* fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(impl_->buf + off);
        names.push_back(narrow(std::wstring(fni->FileName, fni->FileNameLength / sizeof(WCHAR))));
        if (fni->NextEntryOffset == 0) break;
        off += fni->NextEntryOffset;
    }
    impl_->issue();
    return names;
}

// ---------------------------------------------------------------- introspection

FsInfo fs_info(const fs::path& p) {
    FsInfo info;
    std::wstring root = volume_root(p);
    info.mount_point = narrow(root);
    wchar_t label[MAX_PATH + 1] = {}, fsname[MAX_PATH + 1] = {};
    DWORD serial = 0, maxcomp = 0, flags = 0;
    if (GetVolumeInformationW(root.c_str(), label, MAX_PATH + 1, &serial, &maxcomp, &flags, fsname, MAX_PATH + 1)) {
        info.type = narrow(fsname);
        info.volume_label = narrow(label);
        info.max_name_len = maxcomp;
    }
    ULARGE_INTEGER avail{}, total{}, freeb{};
    if (GetDiskFreeSpaceExW(root.c_str(), &avail, &total, &freeb)) {
        info.avail_bytes = avail.QuadPart;
        info.total_bytes = total.QuadPart;
        info.free_bytes = freeb.QuadPart;
    }
    info.block_size = cluster_size(p);
    wchar_t guid[64] = {};
    if (GetVolumeNameForVolumeMountPointW(root.c_str(), guid, 64)) info.device = narrow(guid);

    switch (GetDriveTypeW(root.c_str())) {
        case DRIVE_FIXED: info.features.push_back("drive:fixed"); break;
        case DRIVE_REMOVABLE: info.features.push_back("drive:removable"); break;
        case DRIVE_REMOTE: info.features.push_back("drive:remote"); break;
        case DRIVE_RAMDISK: info.features.push_back("drive:ramdisk"); break;
        case DRIVE_CDROM: info.features.push_back("drive:cdrom"); break;
        default: break;
    }
    static const std::pair<DWORD, const char*> kFlags[] = {
        {0x00000001, "case_sensitive_search"}, {0x00000002, "case_preserved_names"},
        {0x00000004, "unicode_on_disk"},       {0x00000008, "persistent_acls"},
        {0x00000010, "file_compression"},      {0x00000020, "volume_quotas"},
        {0x00000040, "sparse_files"},          {0x00000080, "reparse_points"},
        {0x00000100, "remote_storage"},        {0x00000200, "posix_unlink_rename"},
        {0x00000400, "bypass_io"},             {0x00000800, "streams_snapshots"},
        {0x00008000, "volume_is_compressed"},  {0x00010000, "object_ids"},
        {0x00020000, "encryption"},            {0x00040000, "named_streams"},
        {0x00080000, "read_only_volume"},      {0x00100000, "sequential_write_once"},
        {0x00200000, "transactions"},          {0x00400000, "hard_links"},
        {0x00800000, "extended_attributes"},   {0x01000000, "open_by_file_id"},
        {0x02000000, "usn_journal"},           {0x04000000, "integrity_streams"},
        {0x08000000, "block_refcounting"},     {0x10000000, "sparse_vdl"},
        {0x20000000, "dax_volume"},            {0x40000000, "ghosting"},
    };
    for (auto& [bit, name] : kFlags)
        if (flags & bit) info.features.push_back(name);
    info.mount_options = cat("flags=0x", std::hex, flags, std::dec, " serial=", serial);
    return info;
}

SystemInfo system_info() {
    SystemInfo si;
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (auto fn = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))))
        fn(&vi);
    bool win11 = vi.dwMajorVersion == 10 && vi.dwBuildNumber >= 22000;
    si.os = cat("Windows ", win11 ? "11" : std::to_string(vi.dwMajorVersion), " (build ", vi.dwBuildNumber, ")");
    SYSTEM_INFO sys{};
    GetNativeSystemInfo(&sys);
    const char* arch = sys.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64   ? "x86_64"
                       : sys.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64"
                                                                                     : "x86";
    si.kernel = cat("NT ", vi.dwMajorVersion, ".", vi.dwMinorVersion, ".", vi.dwBuildNumber, " ", arch);
    wchar_t cpu[256] = {};
    DWORD cb = sizeof(cpu);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, cpu, &cb) == ERROR_SUCCESS)
        si.cpu = narrow(cpu);
    si.cpu_count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) si.ram_bytes = ms.ullTotalPhys;
    wchar_t host[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD hl = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(host, &hl)) si.hostname = narrow(host);
    si.privileged = is_privileged();
    return si;
}

bool is_privileged() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION el{};
    DWORD len = 0;
    bool elevated = GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &len) && el.TokenIsElevated;
    CloseHandle(tok);
    return elevated;
}

size_t page_size() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return si.dwPageSize;
}

size_t direct_io_alignment(const fs::path&) { return 4096; }

ErrKind classify(const std::error_code& ec) {
    if (!ec) return ErrKind::None;
    if (ec.category() == std::generic_category()) return classify_errno(ec.value());
    switch (static_cast<DWORD>(ec.value())) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
        case ERROR_BAD_NETPATH: return ErrKind::NotFound;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS: return ErrKind::Exists;
        case ERROR_DIR_NOT_EMPTY: return ErrKind::NotEmpty;
        case ERROR_NOT_SAME_DEVICE: return ErrKind::CrossDevice;
        case ERROR_FILENAME_EXCED_RANGE:
        case ERROR_BUFFER_OVERFLOW: return ErrKind::NameTooLong;
        case ERROR_ACCESS_DENIED:
        case ERROR_PRIVILEGE_NOT_HELD:
        case ERROR_WRITE_PROTECT: return ErrKind::Permission;
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_USER_MAPPED_FILE:
        case ERROR_BUSY: return ErrKind::Busy;
        case ERROR_INVALID_FUNCTION:
        case ERROR_NOT_SUPPORTED:
        case ERROR_CALL_NOT_IMPLEMENTED: return ErrKind::Unsupported;
        case ERROR_INVALID_PARAMETER:
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME: return ErrKind::InvalidArg;
        case ERROR_DIRECTORY: return ErrKind::NotDir;
        case ERROR_CANT_RESOLVE_FILENAME: return ErrKind::Loop;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL: return ErrKind::NoSpace;
        case ERROR_FILE_TOO_LARGE: return ErrKind::TooBig;
        default: break;
    }
    // Fall back on the library's own mapping to POSIX error conditions.
    auto cond = ec.default_error_condition();
    if (cond.category() == std::generic_category()) return classify_errno(cond.value());
    return ErrKind::Other;
}

void platform_init() {
    SetConsoleOutputCP(CP_UTF8);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (out != kInvalid && GetConsoleMode(out, &mode)) SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

const char* platform_name() { return "windows"; }

}  // namespace fsb::plat

#endif  // _WIN32
