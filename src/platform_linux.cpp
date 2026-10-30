// Linux implementation of the platform layer.
#ifdef __linux__

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "platform.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include "common.hpp"

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1 << 1)
#endif
#ifndef FICLONE
#define FICLONE _IOW(0x94, 9, int)
#endif

namespace fsb::plat {

namespace {

[[noreturn]] void throw_errno(int err, const std::string& what, const fs::path& p) {
    throw std::system_error(err, std::generic_category(), what + " '" + path_str(p) + "'");
}

[[noreturn]] void throw_errno(const std::string& what, const fs::path& p) { throw_errno(errno, what, p); }

std::error_code errno_ec(int err = errno) { return {err, std::generic_category()}; }

int64_t ts_ns(const struct statx_timestamp& t) {
    return static_cast<int64_t>(t.tv_sec) * 1000000000LL + t.tv_nsec;
}

FileStat from_statx(const struct statx& s) {
    FileStat st;
    st.size = s.stx_size;
    st.allocated = s.stx_blocks * 512ull;
    st.nlink = s.stx_nlink;
    st.device = (static_cast<uint64_t>(s.stx_dev_major) << 32) | s.stx_dev_minor;
    st.inode = s.stx_ino;
    st.atime_ns = ts_ns(s.stx_atime);
    st.mtime_ns = ts_ns(s.stx_mtime);
    st.ctime_ns = ts_ns(s.stx_ctime);
    if (s.stx_mask & STATX_BTIME) st.btime_ns = ts_ns(s.stx_btime);
    st.is_dir = S_ISDIR(s.stx_mode);
    st.is_regular = S_ISREG(s.stx_mode);
    st.is_symlink = S_ISLNK(s.stx_mode);
    st.mode = s.stx_mode & 07777;
    return st;
}

}  // namespace

// ---------------------------------------------------------------- MappedRegion

MappedRegion::~MappedRegion() { unmap(); }

MappedRegion::MappedRegion(MappedRegion&& o) noexcept
    : addr_(std::exchange(o.addr_, nullptr)), len_(std::exchange(o.len_, 0)) {}

MappedRegion& MappedRegion::operator=(MappedRegion&& o) noexcept {
    if (this != &o) {
        unmap();
        addr_ = std::exchange(o.addr_, nullptr);
        len_ = std::exchange(o.len_, 0);
    }
    return *this;
}

void MappedRegion::flush() {
    if (addr_ && ::msync(addr_, len_, MS_SYNC) != 0)
        throw std::system_error(errno, std::generic_category(), "msync");
}

void MappedRegion::unmap() {
    if (addr_) ::munmap(addr_, len_);
    addr_ = nullptr;
    len_ = 0;
}

// ---------------------------------------------------------------- File

File::~File() { close(); }

File::File(File&& o) noexcept : h_(std::exchange(o.h_, -1)), path_(std::move(o.path_)), flags_(o.flags_) {}

File& File::operator=(File&& o) noexcept {
    if (this != &o) {
        close();
        h_ = std::exchange(o.h_, -1);
        path_ = std::move(o.path_);
        flags_ = o.flags_;
    }
    return *this;
}

bool File::valid() const { return h_ >= 0; }

void File::close() {
    if (h_ >= 0) ::close(h_);
    h_ = -1;
}

File File::open(const fs::path& p, unsigned flags) {
    std::error_code ec;
    File f = open(p, flags, ec);
    if (ec) throw std::system_error(ec, "open '" + path_str(p) + "'");
    return f;
}

File File::open(const fs::path& p, unsigned flags, std::error_code& ec) {
    namespace of = open_flags;
    int oflags = O_CLOEXEC;
    if ((flags & of::read) && (flags & (of::write | of::append))) oflags |= O_RDWR;
    else if (flags & (of::write | of::append)) oflags |= O_WRONLY;
    else oflags |= O_RDONLY;
    if (flags & of::create) oflags |= O_CREAT;
    if (flags & of::exclusive) oflags |= O_EXCL;
    if (flags & of::truncate) oflags |= O_TRUNC;
    if (flags & of::append) oflags |= O_APPEND;
    if (flags & of::direct) oflags |= O_DIRECT;
    if (flags & of::sync) oflags |= O_SYNC;
    if (flags & of::dsync) oflags |= O_DSYNC;

    File f;
    int fd;
    do {
        fd = ::open(p.c_str(), oflags, 0644);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        ec = errno_ec();
        return f;
    }
    if (flags & of::sequential) ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    if (flags & of::random) ::posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
    f.h_ = fd;
    f.path_ = p;
    f.flags_ = flags;
    ec.clear();
    return f;
}

File File::open_dir(const fs::path& p, std::error_code& ec) {
    File f;
    int fd = ::open(p.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        ec = errno_ec();
        return f;
    }
    f.h_ = fd;
    f.path_ = p;
    ec.clear();
    return f;
}

File File::open_temp_anonymous(const fs::path& dir, std::error_code& ec) {
    File f;
    int fd = ::open(dir.c_str(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        ec = errno_ec();
        return f;
    }
    f.h_ = fd;
    f.path_ = dir;
    f.flags_ = open_flags::read | open_flags::write;
    ec.clear();
    return f;
}

size_t File::pread(void* buf, size_t len, uint64_t off) {
    auto* p = static_cast<char*>(buf);
    size_t done = 0;
    while (done < len) {
        ssize_t r = ::pread(h_, p + done, len - done, static_cast<off_t>(off + done));
        if (r < 0) {
            if (errno == EINTR) continue;
            throw_errno("pread", path_);
        }
        if (r == 0) break;
        done += static_cast<size_t>(r);
    }
    return done;
}

size_t File::pwrite(const void* buf, size_t len, uint64_t off) {
    const auto* p = static_cast<const char*>(buf);
    size_t done = 0;
    while (done < len) {
        ssize_t r = ::pwrite(h_, p + done, len - done, static_cast<off_t>(off + done));
        if (r < 0) {
            if (errno == EINTR) continue;
            throw_errno("pwrite", path_);
        }
        done += static_cast<size_t>(r);
    }
    return done;
}

size_t File::read(void* buf, size_t len) {
    auto* p = static_cast<char*>(buf);
    size_t done = 0;
    while (done < len) {
        ssize_t r = ::read(h_, p + done, len - done);
        if (r < 0) {
            if (errno == EINTR) continue;
            throw_errno("read", path_);
        }
        if (r == 0) break;
        done += static_cast<size_t>(r);
    }
    return done;
}

size_t File::write(const void* buf, size_t len) {
    const auto* p = static_cast<const char*>(buf);
    size_t done = 0;
    while (done < len) {
        ssize_t r = ::write(h_, p + done, len - done);
        if (r < 0) {
            if (errno == EINTR) continue;
            throw_errno("write", path_);
        }
        done += static_cast<size_t>(r);
    }
    return done;
}

uint64_t File::seek(int64_t off, int whence) {
    int w = whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END;
    off_t r = ::lseek(h_, off, w);
    if (r < 0) throw_errno("lseek", path_);
    return static_cast<uint64_t>(r);
}

uint64_t File::size() {
    struct stat st;
    if (::fstat(h_, &st) != 0) throw_errno("fstat", path_);
    return static_cast<uint64_t>(st.st_size);
}

void File::truncate(uint64_t n) {
    if (::ftruncate(h_, static_cast<off_t>(n)) != 0) throw_errno("ftruncate", path_);
}

void File::sync() {
    if (::fsync(h_) != 0) throw_errno("fsync", path_);
}

void File::datasync() {
    if (::fdatasync(h_) != 0) throw_errno("fdatasync", path_);
}

FileStat File::stat() {
    struct statx s{};
    if (::statx(h_, "", AT_EMPTY_PATH, STATX_BASIC_STATS | STATX_BTIME, &s) != 0) throw_errno("statx", path_);
    return from_statx(s);
}

void File::preallocate(uint64_t off, uint64_t len, std::error_code& ec) {
    if (::fallocate(h_, FALLOC_FL_KEEP_SIZE, static_cast<off_t>(off), static_cast<off_t>(len)) != 0) ec = errno_ec();
    else ec.clear();
}

void File::set_sparse(std::error_code& ec) { ec.clear(); }

void File::punch_hole(uint64_t off, uint64_t len, std::error_code& ec) {
    if (::fallocate(h_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<off_t>(off),
                    static_cast<off_t>(len)) != 0)
        ec = errno_ec();
    else ec.clear();
}

std::vector<Extent> File::data_ranges(std::error_code& ec) {
    std::vector<Extent> out;
    ec.clear();
    uint64_t end = size();
    off_t pos = 0;
    while (static_cast<uint64_t>(pos) < end) {
        off_t data = ::lseek(h_, pos, SEEK_DATA);
        if (data < 0) {
            if (errno == ENXIO) break;  // no more data
            ec = errno_ec();
            return {};
        }
        off_t hole = ::lseek(h_, data, SEEK_HOLE);
        if (hole < 0) {
            ec = errno_ec();
            return {};
        }
        out.push_back({static_cast<uint64_t>(data), static_cast<uint64_t>(hole - data)});
        pos = hole;
    }
    return out;
}

bool File::try_lock(uint64_t off, uint64_t len, bool exclusive) {
    struct flock fl{};
    fl.l_type = exclusive ? F_WRLCK : F_RDLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = static_cast<off_t>(off);
    fl.l_len = static_cast<off_t>(len);
    if (::fcntl(h_, F_OFD_SETLK, &fl) == 0) return true;
    if (errno == EAGAIN || errno == EACCES) return false;
    throw_errno("fcntl(F_OFD_SETLK)", path_);
}

void File::unlock(uint64_t off, uint64_t len) {
    struct flock fl{};
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = static_cast<off_t>(off);
    fl.l_len = static_cast<off_t>(len);
    if (::fcntl(h_, F_OFD_SETLK, &fl) != 0) throw_errno("fcntl(F_UNLCK)", path_);
}

MappedRegion File::map(uint64_t off, size_t len, bool writable) {
    int prot = PROT_READ | (writable ? PROT_WRITE : 0);
    void* a = ::mmap(nullptr, len, prot, MAP_SHARED, h_, static_cast<off_t>(off));
    if (a == MAP_FAILED) throw_errno("mmap", path_);
    MappedRegion r;
    r.addr_ = a;
    r.len_ = len;
    return r;
}

void File::link_to(const fs::path& p, std::error_code& ec) {
    std::string proc = "/proc/self/fd/" + std::to_string(h_);
    if (::linkat(AT_FDCWD, proc.c_str(), AT_FDCWD, p.c_str(), AT_SYMLINK_FOLLOW) != 0) ec = errno_ec();
    else ec.clear();
}

bool File::drop_cache() {
    ::fdatasync(h_);
    return ::posix_fadvise(h_, 0, 0, POSIX_FADV_DONTNEED) == 0;
}

// ---------------------------------------------------------------- path operations

FileStat stat_path(const fs::path& p, bool follow) {
    struct statx s{};
    int flags = follow ? 0 : AT_SYMLINK_NOFOLLOW;
    if (::statx(AT_FDCWD, p.c_str(), flags, STATX_BASIC_STATS | STATX_BTIME, &s) != 0) throw_errno("statx", p);
    return from_statx(s);
}

void set_file_times(const fs::path& p, int64_t atime_ns, int64_t mtime_ns) {
    auto to_ts = [](int64_t ns) {
        struct timespec ts;
        ts.tv_sec = static_cast<time_t>(ns / 1000000000LL);
        ts.tv_nsec = static_cast<long>(ns % 1000000000LL);
        if (ts.tv_nsec < 0) {
            ts.tv_nsec += 1000000000L;
            ts.tv_sec -= 1;
        }
        return ts;
    };
    struct timespec ts[2] = {to_ts(atime_ns), to_ts(mtime_ns)};
    if (::utimensat(AT_FDCWD, p.c_str(), ts, 0) != 0) throw_errno("utimensat", p);
}

void rename_replace(const fs::path& from, const fs::path& to) {
    if (::rename(from.c_str(), to.c_str()) != 0) throw_errno("rename", from);
}

static int renameat2_sys(const fs::path& a, const fs::path& b, unsigned flags) {
    return static_cast<int>(::syscall(SYS_renameat2, AT_FDCWD, a.c_str(), AT_FDCWD, b.c_str(), flags));
}

void rename_noreplace(const fs::path& from, const fs::path& to, std::error_code& ec) {
    if (renameat2_sys(from, to, RENAME_NOREPLACE) != 0) ec = errno_ec();
    else ec.clear();
}

void rename_exchange(const fs::path& a, const fs::path& b, std::error_code& ec) {
    if (renameat2_sys(a, b, RENAME_EXCHANGE) != 0) ec = errno_ec();
    else ec.clear();
}

void copy_kernel(const fs::path& src, const fs::path& dst) {
    File in = File::open(src, open_flags::read | open_flags::sequential);
    File out = File::open(dst, open_flags::write | open_flags::create | open_flags::truncate);
    uint64_t remaining = in.size();
    bool use_cfr = true;
    while (remaining > 0) {
        size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, 1ull << 30));
        ssize_t n;
        if (use_cfr) {
            n = ::copy_file_range(in.native(), nullptr, out.native(), nullptr, chunk, 0);
            if (n < 0 && (errno == EXDEV || errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP)) {
                use_cfr = false;  // fall back to sendfile
                continue;
            }
        } else {
            n = ::sendfile(out.native(), in.native(), nullptr, chunk);
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            throw_errno(use_cfr ? "copy_file_range" : "sendfile", src);
        }
        if (n == 0) break;
        remaining -= static_cast<uint64_t>(n);
    }
}

void copy_reflink(const fs::path& src, const fs::path& dst, std::error_code& ec) {
    File in = File::open(src, open_flags::read);
    File out = File::open(dst, open_flags::write | open_flags::create | open_flags::truncate);
    if (::ioctl(out.native(), FICLONE, in.native()) != 0) {
        ec = errno_ec();
        out.close();
        std::error_code ignore;
        fs::remove(dst, ignore);
        return;
    }
    ec.clear();
}

// ---------------------------------------------------------------- xattrs

static std::string xname(const std::string& n) { return "user." + n; }

void xattr_set(const fs::path& p, const std::string& name, const std::string& value, std::error_code& ec) {
    if (::setxattr(p.c_str(), xname(name).c_str(), value.data(), value.size(), 0) != 0) ec = errno_ec();
    else ec.clear();
}

std::string xattr_get(const fs::path& p, const std::string& name, std::error_code& ec) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        ssize_t n = ::getxattr(p.c_str(), xname(name).c_str(), nullptr, 0);
        if (n < 0) {
            ec = errno_ec();
            return {};
        }
        std::string v(static_cast<size_t>(n), '\0');
        ssize_t m = ::getxattr(p.c_str(), xname(name).c_str(), v.data(), v.size());
        if (m >= 0) {
            v.resize(static_cast<size_t>(m));
            ec.clear();
            return v;
        }
        if (errno != ERANGE) {
            ec = errno_ec();
            return {};
        }
    }
    ec = errno_ec(ERANGE);
    return {};
}

std::vector<std::string> xattr_list(const fs::path& p, std::error_code& ec) {
    ssize_t n = ::listxattr(p.c_str(), nullptr, 0);
    if (n < 0) {
        ec = errno_ec();
        return {};
    }
    std::string buf(static_cast<size_t>(n) + 1, '\0');
    n = ::listxattr(p.c_str(), buf.data(), buf.size());
    if (n < 0) {
        ec = errno_ec();
        return {};
    }
    std::vector<std::string> out;
    for (size_t i = 0; i < static_cast<size_t>(n);) {
        std::string name(buf.c_str() + i);
        i += name.size() + 1;
        if (name.rfind("user.", 0) == 0) out.push_back(name.substr(5));
    }
    ec.clear();
    return out;
}

void xattr_remove(const fs::path& p, const std::string& name, std::error_code& ec) {
    if (::removexattr(p.c_str(), xname(name).c_str()) != 0) ec = errno_ec();
    else ec.clear();
}

std::string xattr_backend() { return "extended attributes (user.* namespace)"; }

bool drop_file_cache(const fs::path& p) {
    int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ::fdatasync(fd);
    bool ok = ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) == 0;
    ::close(fd);
    return ok;
}

bool sync_filesystem(const fs::path& p) {
    int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = ::syncfs(fd) == 0;
    ::close(fd);
    return ok;
}

// ---------------------------------------------------------------- DirWatcher

struct DirWatcher::Impl {
    int fd = -1;
};

DirWatcher::DirWatcher() : impl_(std::make_unique<Impl>()) {}

DirWatcher::~DirWatcher() {
    if (impl_->fd >= 0) ::close(impl_->fd);
}

void DirWatcher::start(const fs::path& dir, std::error_code& ec) {
    impl_->fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (impl_->fd < 0) {
        ec = errno_ec();
        return;
    }
    uint32_t mask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_ATTRIB;
    if (::inotify_add_watch(impl_->fd, dir.c_str(), mask) < 0) {
        ec = errno_ec();
        return;
    }
    ec.clear();
}

std::vector<std::string> DirWatcher::wait(int timeout_ms) {
    std::vector<std::string> names;
    if (impl_->fd < 0) return names;
    struct pollfd pfd{impl_->fd, POLLIN, 0};
    if (::poll(&pfd, 1, timeout_ms) <= 0) return names;
    alignas(struct inotify_event) char buf[64 * 1024];
    ssize_t n = ::read(impl_->fd, buf, sizeof(buf));
    for (ssize_t i = 0; i < n;) {
        auto* ev = reinterpret_cast<struct inotify_event*>(buf + i);
        names.emplace_back(ev->len ? ev->name : "");
        i += static_cast<ssize_t>(sizeof(struct inotify_event) + ev->len);
    }
    return names;
}

// ---------------------------------------------------------------- introspection

namespace {

std::string unescape_mount(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
            int v = (s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0');
            out.push_back(static_cast<char>(v));
            i += 3;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::string magic_name(unsigned long m) {
    static const std::map<unsigned long, std::string> names = {
        {0xEF53, "ext2/3/4"},       {0x58465342, "xfs"},       {0x9123683E, "btrfs"},
        {0x01021994, "tmpfs"},      {0x2FC12FC1, "zfs"},       {0xF2F52010, "f2fs"},
        {0x7366746E, "ntfs3"},      {0x5346544E, "ntfs"},      {0x4D44, "vfat"},
        {0x2011BAB0, "exfat"},      {0x6969, "nfs"},           {0xFF534D42, "cifs"},
        {0xFE534D42, "smb2"},       {0x794C7630, "overlayfs"}, {0x65735546, "fuse"},
        {0xCA451A4E, "bcachefs"},   {0x01021997, "9p"},        {0x73717368, "squashfs"},
        {0x3153464A, "jfs"},        {0x52654973, "reiserfs"},  {0x858458F6, "ramfs"},
        {0x24051905, "ubifs"},      {0xF15F, "ecryptfs"},      {0x47504653, "gpfs"},
        {0x0BD00BD0, "lustre"},     {0x00C36400, "ceph"},      {0x6E736673, "nsfs"},
    };
    auto it = names.find(m);
    return it != names.end() ? it->second : cat("unknown(0x", std::hex, m, ")");
}

std::string read_first_line_matching(const char* file, const std::string& key) {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(key, 0) == 0) return line;
    }
    return {};
}

}  // namespace

FsInfo fs_info(const fs::path& p) {
    FsInfo info;
    struct statfs sfs{};
    if (::statfs(p.c_str(), &sfs) == 0) {
        info.type = magic_name(static_cast<unsigned long>(sfs.f_type));
        info.block_size = static_cast<uint64_t>(sfs.f_bsize);
    }
    struct statvfs svfs{};
    if (::statvfs(p.c_str(), &svfs) == 0) {
        uint64_t frsize = svfs.f_frsize ? svfs.f_frsize : svfs.f_bsize;
        info.total_bytes = svfs.f_blocks * frsize;
        info.free_bytes = svfs.f_bfree * frsize;
        info.avail_bytes = svfs.f_bavail * frsize;
        info.total_inodes = svfs.f_files;
        info.free_inodes = svfs.f_ffree;
        info.max_name_len = svfs.f_namemax;
    }

    // Find the mount that contains the path (longest matching mount point).
    std::error_code ec;
    fs::path canon = fs::canonical(p, ec);
    std::string target = ec ? path_str(p) : path_str(canon);
    std::ifstream mi("/proc/self/mountinfo");
    std::string line;
    size_t best = 0;
    while (std::getline(mi, line)) {
        std::istringstream ls(line);
        std::vector<std::string> f;
        std::string tok;
        while (ls >> tok) f.push_back(tok);
        auto dash = std::find(f.begin(), f.end(), "-");
        if (f.size() < 7 || dash == f.end() || std::distance(dash, f.end()) < 3) continue;
        std::string mp = unescape_mount(f[4]);
        bool match = target == mp || mp == "/" ||
                     (target.rfind(mp, 0) == 0 && target.size() > mp.size() && target[mp.size()] == '/');
        if (!match || mp.size() < best) continue;
        best = mp.size();
        info.mount_point = mp;
        info.type = *(dash + 1);
        info.device = unescape_mount(*(dash + 2));
        std::string opts = f[5];
        if (dash + 3 < f.end()) opts += "," + *(dash + 3);
        info.mount_options = opts;
    }
    info.features.clear();
    std::vector<std::string> seen;
    for (auto& o : split(info.mount_options, ',')) {
        if (std::find(seen.begin(), seen.end(), o) == seen.end()) seen.push_back(o);
    }
    info.features = seen;
    return info;
}

SystemInfo system_info() {
    SystemInfo si;
    struct utsname u{};
    if (::uname(&u) == 0) {
        si.kernel = std::string(u.sysname) + " " + u.release + " " + u.machine;
        si.hostname = u.nodename;
    }
    std::string pretty = read_first_line_matching("/etc/os-release", "PRETTY_NAME=");
    if (!pretty.empty()) {
        pretty = pretty.substr(12);
        pretty.erase(std::remove(pretty.begin(), pretty.end(), '"'), pretty.end());
        si.os = pretty;
    } else {
        si.os = "Linux";
    }
    std::string cpu = read_first_line_matching("/proc/cpuinfo", "model name");
    if (auto pos = cpu.find(':'); pos != std::string::npos) si.cpu = cpu.substr(pos + 2);
    si.cpu_count = static_cast<unsigned>(::sysconf(_SC_NPROCESSORS_ONLN));
    struct sysinfo s{};
    if (::sysinfo(&s) == 0) si.ram_bytes = static_cast<uint64_t>(s.totalram) * s.mem_unit;
    si.privileged = is_privileged();
    return si;
}

bool is_privileged() { return ::geteuid() == 0; }

size_t page_size() { return static_cast<size_t>(::sysconf(_SC_PAGESIZE)); }

size_t direct_io_alignment(const fs::path& p) {
    size_t align = 4096;
#ifdef STATX_DIOALIGN
    struct statx s{};
    if (::statx(AT_FDCWD, p.c_str(), 0, STATX_DIOALIGN, &s) == 0 && (s.stx_mask & STATX_DIOALIGN)) {
        align = std::max<size_t>(align, s.stx_dio_offset_align);
        align = std::max<size_t>(align, s.stx_dio_mem_align);
    }
#else
    (void)p;
#endif
    return align;
}

ErrKind classify(const std::error_code& ec) {
    if (!ec) return ErrKind::None;
    return classify_errno(ec.value());
}

void platform_init() {}

const char* platform_name() { return "linux"; }

}  // namespace fsb::plat

#endif  // __linux__
