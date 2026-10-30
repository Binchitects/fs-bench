// Thin OS abstraction over the file APIs the suite exercises.
//
// Everything that has a portable std::filesystem equivalent (mkdir, directory
// iteration, hard/symbolic links, remove_all, ...) is used directly by the
// tests. This layer covers what std::filesystem does not: positional and direct
// I/O, sync, preallocation, sparse files, locks, mmap, extended attributes /
// alternate data streams, kernel copy and reflink, change notification and
// filesystem/volume introspection.
//
// Errors are reported by throwing std::system_error (for unexpected failures)
// or by returning a std::error_code (for probes where failure is expected).
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fsb::plat {

namespace fs = std::filesystem;

namespace open_flags {
constexpr unsigned read = 1u << 0;
constexpr unsigned write = 1u << 1;
constexpr unsigned create = 1u << 2;      // create if missing
constexpr unsigned exclusive = 1u << 3;   // with create: fail if it exists
constexpr unsigned truncate = 1u << 4;
constexpr unsigned append = 1u << 5;      // every write goes to end of file
constexpr unsigned direct = 1u << 6;      // bypass page cache (O_DIRECT / NO_BUFFERING)
constexpr unsigned sync = 1u << 7;        // write-through (O_SYNC / WRITE_THROUGH)
constexpr unsigned dsync = 1u << 8;       // data-sync writes (O_DSYNC / WRITE_THROUGH)
constexpr unsigned sequential = 1u << 9;  // access-pattern hint
constexpr unsigned random = 1u << 10;     // access-pattern hint
}  // namespace open_flags

constexpr unsigned kRW = open_flags::read | open_flags::write;
constexpr unsigned kCreateRW = kRW | open_flags::create;
constexpr unsigned kCreateTruncRW = kRW | open_flags::create | open_flags::truncate;

// Timestamps are nanoseconds since the Unix epoch (may be negative).
struct FileStat {
    uint64_t size = 0;
    uint64_t allocated = 0;  // bytes actually allocated on disk
    uint64_t nlink = 0;
    uint64_t device = 0;     // st_dev / volume serial
    uint64_t inode = 0;      // st_ino / file index (low 64 bits of file id)
    int64_t atime_ns = 0, mtime_ns = 0, ctime_ns = 0;
    std::optional<int64_t> btime_ns;  // birth/creation time if the FS reports it
    bool is_dir = false, is_symlink = false, is_regular = false;
    uint32_t mode = 0;        // POSIX mode bits (synthesised on Windows)
    uint32_t attributes = 0;  // Windows FILE_ATTRIBUTE_* (0 on POSIX)
};

struct Extent {
    uint64_t offset;
    uint64_t length;
};

class MappedRegion {
public:
    MappedRegion() = default;
    ~MappedRegion();
    MappedRegion(MappedRegion&& o) noexcept;
    MappedRegion& operator=(MappedRegion&& o) noexcept;
    MappedRegion(const MappedRegion&) = delete;
    MappedRegion& operator=(const MappedRegion&) = delete;

    unsigned char* data() const { return static_cast<unsigned char*>(addr_); }
    size_t size() const { return len_; }
    void flush();  // msync / FlushViewOfFile (+ flush of the file)
    void unmap();

private:
    friend class File;
    void* addr_ = nullptr;
    size_t len_ = 0;
    void* mapping_ = nullptr;  // Windows section handle
    void* file_ = nullptr;     // Windows file handle used for FlushFileBuffers
};

class File {
public:
#ifdef _WIN32
    using Handle = void*;
#else
    using Handle = int;
#endif

    File() = default;
    ~File();
    File(File&& o) noexcept;
    File& operator=(File&& o) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    // Throws std::system_error on failure.
    static File open(const fs::path& p, unsigned flags);
    // Non-throwing variant.
    static File open(const fs::path& p, unsigned flags, std::error_code& ec);
    // Opens a directory handle (for fsync of directory metadata etc.).
    static File open_dir(const fs::path& p, std::error_code& ec);
    // Anonymous temporary file in `dir` that is never visible / vanishes on close
    // (Linux O_TMPFILE, Windows FILE_FLAG_DELETE_ON_CLOSE).
    static File open_temp_anonymous(const fs::path& dir, std::error_code& ec);

    bool valid() const;
    void close();
    Handle native() const { return h_; }
    const fs::path& path() const { return path_; }

    size_t pread(void* buf, size_t len, uint64_t off);         // short read only at EOF
    size_t pwrite(const void* buf, size_t len, uint64_t off);  // writes everything
    size_t read(void* buf, size_t len);                        // at current position
    size_t write(const void* buf, size_t len);                 // at current position (or EOF in append mode)
    uint64_t seek(int64_t off, int whence);                    // 0=SET 1=CUR 2=END

    uint64_t size();
    void truncate(uint64_t new_size);
    void sync();      // fsync / FlushFileBuffers
    void datasync();  // fdatasync / FlushFileBuffers
    FileStat stat();

    // Reserve space without changing the file size.
    void preallocate(uint64_t off, uint64_t len, std::error_code& ec);
    void set_sparse(std::error_code& ec);  // no-op on Linux
    void punch_hole(uint64_t off, uint64_t len, std::error_code& ec);
    std::vector<Extent> data_ranges(std::error_code& ec);  // SEEK_DATA / ALLOCATED_RANGES

    // Byte-range locks, per handle (Linux OFD locks / Windows LockFileEx).
    // Returns false if the range is held by a conflicting lock.
    bool try_lock(uint64_t off, uint64_t len, bool exclusive);
    void unlock(uint64_t off, uint64_t len);

    MappedRegion map(uint64_t off, size_t len, bool writable);

    // Link an anonymous (O_TMPFILE) file into the namespace.
    void link_to(const fs::path& p, std::error_code& ec);

    // Hint the OS to drop cached pages of this file (best effort).
    bool drop_cache();

private:
    Handle h_ =
#ifdef _WIN32
        reinterpret_cast<void*>(static_cast<intptr_t>(-1));
#else
        -1;
#endif
    fs::path path_;
    unsigned flags_ = 0;
};

// ---------------------------------------------------------------- path operations

FileStat stat_path(const fs::path& p, bool follow_symlinks = true);
void set_file_times(const fs::path& p, int64_t atime_ns, int64_t mtime_ns);

void rename_replace(const fs::path& from, const fs::path& to);  // atomic replace
void rename_noreplace(const fs::path& from, const fs::path& to, std::error_code& ec);
void rename_exchange(const fs::path& a, const fs::path& b, std::error_code& ec);

// Copy helpers; all of them create/overwrite `dst`.
void copy_readwrite(const fs::path& src, const fs::path& dst, size_t bufsize, bool direct = false);
void copy_kernel(const fs::path& src, const fs::path& dst);  // copy_file_range / CopyFileExW
void copy_reflink(const fs::path& src, const fs::path& dst, std::error_code& ec);  // FICLONE / block clone
void copy_mmap(const fs::path& src, const fs::path& dst);

// Move that falls back to copy+delete across devices.
void move_path(const fs::path& from, const fs::path& to);

// Extended attributes on Linux, alternate data streams on Windows.
void xattr_set(const fs::path& p, const std::string& name, const std::string& value, std::error_code& ec);
std::string xattr_get(const fs::path& p, const std::string& name, std::error_code& ec);
std::vector<std::string> xattr_list(const fs::path& p, std::error_code& ec);
void xattr_remove(const fs::path& p, const std::string& name, std::error_code& ec);
std::string xattr_backend();  // "xattr (user.*)" / "NTFS alternate data streams"

// Drop the OS cache for a file (open + flush + fadvise; best effort on Windows).
bool drop_file_cache(const fs::path& p);

// Flush all dirty data of the filesystem containing `p` (syncfs; volume flush on
// Windows, which needs administrator rights). Returns false if not possible.
bool sync_filesystem(const fs::path& p);

// Directory change notification (inotify / ReadDirectoryChangesW).
class DirWatcher {
public:
    DirWatcher();
    ~DirWatcher();
    DirWatcher(const DirWatcher&) = delete;
    DirWatcher& operator=(const DirWatcher&) = delete;
    void start(const fs::path& dir, std::error_code& ec);
    // Waits up to timeout_ms for at least one event. Returns the names seen.
    std::vector<std::string> wait(int timeout_ms);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------- introspection

struct FsInfo {
    std::string type;          // ext4, xfs, btrfs, NTFS, ReFS, ...
    std::string device;        // /dev/nvme0n1p2, \\?\Volume{...}
    std::string mount_point;
    std::string mount_options;
    std::string volume_label;
    uint64_t block_size = 0;   // fundamental block / cluster size
    uint64_t total_bytes = 0, free_bytes = 0, avail_bytes = 0;
    uint64_t total_inodes = 0, free_inodes = 0;
    uint64_t max_name_len = 0;
    std::vector<std::string> features;  // volume capability flags (Windows) / mount flags
};

FsInfo fs_info(const fs::path& p);

struct SystemInfo {
    std::string os, kernel, cpu, hostname;
    unsigned cpu_count = 0;
    uint64_t ram_bytes = 0;
    bool privileged = false;  // root / elevated admin
};

SystemInfo system_info();
bool is_privileged();
size_t page_size();
size_t direct_io_alignment(const fs::path& p);

// Portable classification of errno / Win32 error codes.
enum class ErrKind {
    None, NotFound, Exists, NotEmpty, CrossDevice, NameTooLong, Permission, Busy,
    Unsupported, InvalidArg, NotDir, IsDir, Loop, NoSpace, TooBig, Other
};
ErrKind classify(const std::error_code& ec);
ErrKind classify_errno(int e);
const char* err_kind_name(ErrKind k);
inline std::ostream& operator<<(std::ostream& os, ErrKind k) { return os << err_kind_name(k); }

// True if the error means "the OS/filesystem doesn't support this".
bool is_unsupported(const std::error_code& ec);
// True if the error means "permission denied / privilege missing".
bool is_permission_error(const std::error_code& ec);

void platform_init();  // console setup etc.
const char* platform_name();  // "linux", "windows", ...

}  // namespace fsb::plat
