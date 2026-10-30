# fsbench — filesystem test suite & benchmark

A single native C++20 binary for **Linux and Windows** that

1. **verifies filesystem behaviour**: 91 functional tests covering I/O semantics, metadata, directories, names, copy / move / rename / delete, links, advanced features and concurrency;
2. **builds a feature matrix** showing what the filesystem supports (sparse files, reflinks, xattrs/ADS, case sensitivity, timestamp resolution, …);
3. **benchmarks performance**: sequential and random I/O, thread scaling, sync latency, metadata rates, small files, and copy/move/delete of files and trees;
4. **compares filesystems side by side**: pass several `--target`s and get one comparison table;
5. writes **JSON, CSV and Markdown** reports.

## Build

Requires a C++20 compiler (GCC ≥ 11, Clang ≥ 14, or MSVC 2019 16.10+ / 2022). CMake is optional.

**Linux**
```bash
./build.sh            # CMake if installed, otherwise g++ directly → build/fsbench
```

**Windows** (from a *Developer Command Prompt for VS*)
```bat
build.bat             :: CMake if installed, otherwise cl.exe → build\fsbench.exe
```

**Any platform with CMake**
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release
```

**Cross-compile a Windows .exe from Linux** (with [Zig](https://ziglang.org) as the C++ toolchain)
```bash
zig c++ -target x86_64-windows-gnu -std=c++20 -O2 -Isrc src/*.cpp src/tests/*.cpp src/bench/*.cpp -ladvapi32 -o build-win/fsbench.exe
```

The GitHub Actions workflow in `.github/workflows/ci.yml` builds and runs the suite on Ubuntu and Windows (MSVC, NTFS).

## Usage

```
fsbench [all|test|bench|list|info] [options]
```

| Command | What it does |
|---|---|
| `all` (default) | functional tests, then benchmarks |
| `test` | functional tests only |
| `bench` | benchmarks only |
| `list` | list every test and benchmark with a description |
| `info` | system and filesystem information for the targets |

Key options (`fsbench --help` lists all of them):

| Option | Meaning |
|---|---|
| `-t, --target DIR` | directory on the filesystem under test; repeat to compare filesystems |
| `-a, --alt-target DIR` | directory on a **different** filesystem; enables the cross-device move/copy tests and benchmarks |
| `-p, --profile` | `quick` (≈1–2 min) · `standard` (default) · `full` |
| `-f, --filter` / `-x, --exclude` | comma-separated patterns on `category/name`, e.g. `io/*,copy` |
| `--file-size`, `--duration`, `--block-sizes`, `--rand-block-sizes`, `--threads`, `--io-modes`, `--files`, `--tree-files` | override the profile |
| `--json FILE`, `--csv FILE`, `--md FILE` | reports (`--csv` also writes `FILE_tests.csv`) |
| `--keep` | keep the scratch files |

Examples:
```bash
# Full functional check of a disk
fsbench test -t /mnt/data

# Compare three filesystems
fsbench bench -t /mnt/ext4 -t /mnt/xfs -t /mnt/btrfs --json compare.json --md compare.md

# Windows: NTFS vs ReFS/Dev Drive, with cross-volume moves to E:
fsbench all -t C:\fsb -t D:\fsb -a E:\fsb --profile quick --md report.md

# Only random I/O, deeper queue via threads, 10 s per point
fsbench bench -t /mnt/nvme -f "io/rand*" --threads 1,8,32 --duration 10s
```

All work happens in a fresh `fsbench-<pid>-<time>` directory under each target, which is removed at the end unless you pass `--keep`. The exit code is `0` when everything passes, `1` if any test fails or any benchmark errors, and `2` for usage errors.

### Profiles

| | data size | run time | seq block sizes | rand block sizes | threads | metadata files | tree files |
|---|---|---|---|---|---|---|---|
| quick | 256 MiB | 2 s | 1M | 4K | 1, 4 | 2 000 | 1 000 |
| standard | 1 GiB | 5 s | 64K, 1M | 4K | 1, 4, 16 | 10 000 | 5 000 |
| full | 4 GiB | 10 s | 4K, 64K, 1M, 8M | 4K, 64K | 1 … 32 | 50 000 | 20 000 |

The data size shrinks automatically if the target lacks free space (about 3× the data size is needed).

## Functional tests

Each test reports **PASS**, **FAIL**, **N/A** (the filesystem or OS doesn't offer the feature) or **SKIP** (a precondition is missing, e.g. no `--alt-target`, or running as root).

| Category | Tests |
|---|---|
| `io` | create, round-trip at many sizes, random pwrite/pread against an in-memory model, cursor I/O, append mode, exclusive create, missing files, truncate shrink/grow, write past EOF (zero-filled gap), read past EOF, multi-handle coherence, fsync/fdatasync/directory fsync, O_SYNC/O_DSYNC, files > 4 GiB, 64 MiB integrity stress, mmap coherence, direct I/O, anonymous temp files (O_TMPFILE / delete-on-close) |
| `metadata` | stat consistency, mtime on write/truncate, directory mtime, timestamp resolution and range (pre-1970, post-2038), atime policy (strict/relatime/noatime), birth time, ctime, read-only enforcement, POSIX mode bits, inode stability, space accounting |
| `dirs` | mkdir/rmdir, non-empty rmdir, 64-level nesting, 5 000-entry enumeration, entry types, delete-while-iterating, directory link counts, long paths (260 / 1100 / 3900 chars) |
| `names` | Unicode in 10 scripts incl. emoji and NFC/NFD, special characters, Windows-reserved characters, DOS device names, max name length (ASCII / 2-byte / 4-byte chars), case sensitivity and preservation, case-only rename |
| `ops` | **rename**: same/cross dir, overwrite, *atomic replace under a concurrent reader*, no-replace, exchange, directories, invalid directory renames, open files, self-rename. **move**: same filesystem, cross-device (copy+delete fallback). **copy**: every method (read/write, direct, kernel `copy_file_range`/`CopyFileEx`, mmap, `std::filesystem`), reflink/block clone, overwrite options, copy onto itself, metadata preservation, recursive tree copy, 256 MiB integrity. **delete**: files, open files (POSIX vs delete-pending), read-only files, mapped files, trees |
| `links` | hard links (basic, 1 000 per file, restrictions, same-inode rename), symlinks to files/directories, dangling, loops, relative links moved with their targets, long targets |
| `features` | sparse files + SEEK_HOLE/allocated ranges, hole punching, preallocation, xattrs (Linux) / alternate data streams (Windows) incl. size/count limits, byte-range locks, change notification (inotify / ReadDirectoryChangesW), volume info |
| `concurrency` | parallel writers, interleaved stripes of one file, O_APPEND atomicity, exclusive-create race, mkdir race, rename race, torn-read detection |

## Benchmarks

| Category | Benchmark | Measures |
|---|---|---|
| `io` | `seq_write`, `seq_overwrite`, `seq_read`, `seq_read_cached` | single-stream throughput per block size and I/O mode; writes include the final fsync |
| | `seq_write_mt`, `seq_read_mt` | aggregate throughput with N parallel streams |
| | `rand_read`, `rand_write`, `rand_mixed` | IOPS and latency percentiles per block size × threads × mode (70/30 mix for `rand_mixed`) |
| | `mmap_read` | page-fault-driven read throughput |
| `latency` | `sync_write` | durable 4 KiB writes: append+fsync, append+fdatasync, overwrite+fdatasync, O_DSYNC, direct+O_DSYNC (the database WAL pattern) |
| `meta` | `file_ops` | create, stat, open/close, readdir, set_times, rename, cross-dir rename and delete rates for N files |
| | `small_files` | 4 KiB create+write, read, create+write+fsync, delete |
| | `mkdir_rmdir`, `links`, `parallel_create`, `tree_walk` | directory ops, hard/symlink creation, shared-dir vs per-thread-dir contention, deep tree traversal |
| `ops` | `copy_file` | one large file copied with every method incl. reflink (cold source, destination fsync included) |
| | `copy_tree` | mixed-size tree via `std::filesystem::copy` vs N-thread kernel copy |
| | `delete_tree`, `delete_large` | `remove_all` rate; unlink/truncate time of a large allocated file |
| | `atomic_save` | write-temp + fsync + rename + directory fsync (crash-safe save) |
| | `cross_device` | copy/move of a large file and a tree to `--alt-target` |

Every throughput and IOPS result also records latency (avg, p50, p90, p99, p99.9, max) per operation. Write data is incompressible and each block is unique, so compression and dedup don't inflate the numbers.

### Reading the numbers

- **Buffered vs direct**: `--io-modes buffered,direct` (the default) runs both. *Direct* (`O_DIRECT` / `FILE_FLAG_NO_BUFFERING`) bypasses the page cache and shows what the filesystem and device actually deliver. *Buffered* is what normal applications see.
- **Cache**: before cold buffered reads, the suite evicts the file from the page cache (`posix_fadvise(DONTNEED)` on Linux; on Windows it opens a non-cached handle, which purges cached data for most files). Random buffered reads warm the cache as they run, and the result is labelled accordingly. Use a data size larger than RAM, or direct mode, for device-bound numbers.
- **Durability**: writes include a final fsync in their timing, so buffered results aren't just memory copies.
- Run benchmarks on an otherwise idle system and repeat runs to judge variance.

## Platform notes

| Topic | Linux | Windows |
|---|---|---|
| Direct I/O | `O_DIRECT` (4 KiB alignment, or `STATX_DIOALIGN`) | `FILE_FLAG_NO_BUFFERING` |
| Kernel copy | `copy_file_range` → `sendfile` fallback | `CopyFileExW` |
| Reflink | `FICLONE` (btrfs, XFS, bcachefs, …) | `FSCTL_DUPLICATE_EXTENTS_TO_FILE` (ReFS / Dev Drive) |
| Extended attributes | `user.*` xattrs | NTFS alternate data streams |
| Locks | OFD byte-range locks (per handle) | `LockFileEx` |
| Rename-replace | `rename(2)` / `renameat2` | `FileRenameInfoEx` with POSIX semantics, falling back to `MoveFileExW` |
| Long paths | up to `PATH_MAX` (4096) | `\\?\` paths internally; `std::filesystem` needs `LongPathsEnabled` |
| Symlinks | always | need Developer Mode or an elevated prompt (otherwise SKIP) |

- On Linux, `metadata/readonly_enforced` is skipped when running as root, because root bypasses permission checks.
- `sync_filesystem` (used by tree benchmarks) is `syncfs` on Linux. On Windows it needs administrator rights to flush the volume; without them the result is marked "no fs-wide sync available".

## Project layout

```
src/
  common.*            timing, RNG, hashing, latency statistics, formatting
  platform.hpp        OS abstraction (File, stat, xattr, copy, rename, fs info …)
  platform_linux.cpp  Linux implementation
  platform_windows.cpp Windows implementation
  platform_common.cpp portable helpers + error classification
  framework.*         registry, runner, contexts, CHECK macros
  report.*            console output, JSON / CSV / Markdown writers
  main.cpp            CLI
  tests/*.cpp         functional tests (FSB_TEST)
  bench/*.cpp         benchmarks (FSB_BENCH)
```

### Adding a test or benchmark

```cpp
FSB_TEST(io, my_case, "one-line description") {
    auto p = ctx.path("file.bin");          // inside a fresh, per-test directory
    tu::write_file(p, "data");
    CHECK(tu::read_file(p) == "data");
    CHECK_ERR(plat::File::open(ctx.path("missing"), plat::open_flags::read), NotFound);
    ctx.feature("My feature", "yes");       // shows up in the feature matrix
}

FSB_BENCH(io, my_bench, "description") {
    BenchResult r; /* fill seconds, bytes, ops, lat */ 
    r.category = "io"; r.name = "my_bench"; r.params = "bs=4K";
    ctx.report(r);
}
```

Drop the file in `src/tests/` or `src/bench/`. Both build scripts pick it up automatically.
## License

AGPL-3.0, see [LICENSE](LICENSE).

