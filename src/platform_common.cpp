// Platform-independent helpers built on top of the File abstraction.
#include <algorithm>
#include <cerrno>

#include "common.hpp"
#include "platform.hpp"

namespace fsb::plat {

ErrKind classify_errno(int e) {
    switch (e) {
        case 0: return ErrKind::None;
        case ENOENT: return ErrKind::NotFound;
        case EEXIST: return ErrKind::Exists;
        case ENOTEMPTY: return ErrKind::NotEmpty;
        case EXDEV: return ErrKind::CrossDevice;
        case ENAMETOOLONG: return ErrKind::NameTooLong;
        case EPERM:
        case EACCES:
        case EROFS: return ErrKind::Permission;
        case EBUSY:
        case ETXTBSY: return ErrKind::Busy;
#if defined(ENOTSUP) && defined(EOPNOTSUPP) && ENOTSUP != EOPNOTSUPP
        case EOPNOTSUPP:
#endif
        case ENOTSUP:
        case ENOSYS:
        case ENOTTY: return ErrKind::Unsupported;
        case EINVAL: return ErrKind::InvalidArg;
        case ENOTDIR: return ErrKind::NotDir;
        case EISDIR: return ErrKind::IsDir;
        case ELOOP: return ErrKind::Loop;
        case ENOSPC:
#ifdef EDQUOT
        case EDQUOT:
#endif
            return ErrKind::NoSpace;
        case EFBIG: return ErrKind::TooBig;
        default: return ErrKind::Other;
    }
}

const char* err_kind_name(ErrKind k) {
    switch (k) {
        case ErrKind::None: return "none";
        case ErrKind::NotFound: return "not-found";
        case ErrKind::Exists: return "exists";
        case ErrKind::NotEmpty: return "not-empty";
        case ErrKind::CrossDevice: return "cross-device";
        case ErrKind::NameTooLong: return "name-too-long";
        case ErrKind::Permission: return "permission";
        case ErrKind::Busy: return "busy";
        case ErrKind::Unsupported: return "unsupported";
        case ErrKind::InvalidArg: return "invalid-argument";
        case ErrKind::NotDir: return "not-a-directory";
        case ErrKind::IsDir: return "is-a-directory";
        case ErrKind::Loop: return "symlink-loop";
        case ErrKind::NoSpace: return "no-space";
        case ErrKind::TooBig: return "file-too-large";
        case ErrKind::Other: return "other";
    }
    return "?";
}

bool is_unsupported(const std::error_code& ec) { return classify(ec) == ErrKind::Unsupported; }

bool is_permission_error(const std::error_code& ec) { return classify(ec) == ErrKind::Permission; }

void copy_readwrite(const fs::path& src, const fs::path& dst, size_t bufsize, bool direct) {
    using namespace open_flags;
    const size_t align = 4096;
    if (direct) bufsize = (bufsize + align - 1) / align * align;
    File in = File::open(src, read | sequential | (direct ? open_flags::direct : 0u));
    File out = File::open(dst, write | create | truncate | (direct ? open_flags::direct : 0u));
    uint64_t total = in.size();
    AlignedBuffer buf(bufsize, align);
    uint64_t off = 0;
    while (off < total) {
        size_t want = static_cast<size_t>(std::min<uint64_t>(bufsize, total - off));
        size_t io = direct ? (want + align - 1) / align * align : want;
        size_t got = in.pread(buf.data(), io, off);
        if (got == 0) break;
        size_t wlen = direct ? (got + align - 1) / align * align : got;
        if (wlen > got) std::fill(buf.data() + got, buf.data() + wlen, 0);
        out.pwrite(buf.data(), wlen, off);
        off += got;
    }
    if (direct) out.truncate(total);
}

void copy_mmap(const fs::path& src, const fs::path& dst) {
    using namespace open_flags;
    File in = File::open(src, read);
    File out = File::open(dst, write | create | truncate);
    uint64_t total = in.size();
    if (total == 0) return;
    const uint64_t window = 256ull << 20;  // map in 256 MiB windows (64 KiB aligned)
    for (uint64_t off = 0; off < total; off += window) {
        size_t len = static_cast<size_t>(std::min(window, total - off));
        MappedRegion m = in.map(off, len, false);
        out.pwrite(m.data(), len, off);
    }
}

void move_path(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) return;
    if (classify(ec) != ErrKind::CrossDevice) throw fs::filesystem_error("rename", from, to, ec);
    // Different filesystem: copy then delete the source.
    if (fs::is_directory(fs::symlink_status(from))) {
        fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
        fs::remove_all(from);
    } else {
        fs::copy_file(from, to, fs::copy_options::overwrite_existing);
        fs::remove(from);
    }
}

}  // namespace fsb::plat
