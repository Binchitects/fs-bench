# Filesystem test & benchmark report

- **Started:** 2026-10-30T13:20:42  
- **OS:** Ubuntu 26.04.1 LTS (Linux 7.0.0-38-generic x86_64)  
- **CPU:** 13th Gen Intel(R) Core(TM) i7-13700K (24 logical)  
- **RAM:** 61.49 GiB

## Targets

| Path | Filesystem | Device | Block | Capacity | Options |
|---|---|---|---|---|---|
| /home/ghadyani/disk-test/.scratch | ext4 | /dev/nvme1n1p2 | 4096 | 1.79 TiB | rw,relatime,rw |

## Functional tests

| Target | Test | Status | Details |
|---|---|---|---|
| /home/ghadyani/disk-test/.scratch | `io/create_empty` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `io/write_read_roundtrip` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `io/positional_io` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `io/sequential_cursor` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `io/append_mode` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `io/exclusive_create` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `io/open_missing` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `io/truncate_shrink_grow` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `io/truncate_on_open` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `io/write_past_eof` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `io/read_past_eof` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `io/multi_handle_coherence` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `io/fsync_durability_api` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `io/sync_write_flags` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `io/large_file_4g` | PASS | allocated after 4 KiB write at 5 GiB: 4.00 KiB |
| /home/ghadyani/disk-test/.scratch | `io/integrity_stress` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `io/mmap_coherence` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `io/direct_io` | PASS | unaligned direct write: rejected (Invalid argument) |
| /home/ghadyani/disk-test/.scratch | `io/anonymous_tmpfile` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `metadata/stat_basics` | PASS | 12345-byte file allocates 16384 bytes |
| /home/ghadyani/disk-test/.scratch | `metadata/mtime_on_write` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `metadata/dir_mtime_on_change` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `metadata/timestamp_resolution` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `metadata/atime_policy` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `metadata/birth_time` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `metadata/ctime_on_change` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `metadata/readonly_enforced` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `metadata/posix_mode_bits` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `metadata/file_identity` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `metadata/space_accounting` | PASS | free space dropped by 64.00 MiB after writing 64 MiB (other activity on the volume affects this) |
| /home/ghadyani/disk-test/.scratch | `dirs/mkdir_rmdir` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `dirs/rmdir_nonempty` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `dirs/nested_create` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `dirs/enumerate_many` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `dirs/entry_types` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `dirs/delete_while_iterating` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `dirs/dir_link_count` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `dirs/long_paths` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `names/unicode` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `names/special_chars` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `names/reserved_chars` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `names/reserved_device_names` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `names/max_name_length` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `names/case_sensitivity` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `names/case_only_rename` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `ops/rename_file` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `ops/rename_overwrite` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `ops/rename_atomic_replace` | PASS | 247 reads, 0 missing, 0 torn, 0 open/read errors, 0 failed renames |
| /home/ghadyani/disk-test/.scratch | `ops/rename_noreplace` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `ops/rename_exchange` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `ops/rename_directory` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `ops/rename_dir_edge_cases` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `ops/rename_open_file` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `ops/rename_same_path` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `ops/move_tree_same_fs` | PASS | moved 300 files in 16.2 us |
| /home/ghadyani/disk-test/.scratch | `ops/move_cross_device` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `ops/copy_methods` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `ops/copy_reflink` | N/A | reflink not supported: Operation not supported |
| /home/ghadyani/disk-test/.scratch | `ops/copy_overwrite_options` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `ops/copy_onto_self` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `ops/copy_metadata` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `ops/copy_tree` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `ops/copy_large_integrity` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `ops/delete_file` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `ops/delete_open_file` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `ops/delete_readonly` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `ops/delete_mapped_file` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `ops/delete_tree` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `links/hardlink_basic` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `links/hardlink_many` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `links/hardlink_restrictions` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `links/symlink_file` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `links/symlink_dir` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `links/symlink_dangling` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `links/symlink_loop` | PASS | loop error: symlink-loop (Too many levels of symbolic links) |
| /home/ghadyani/disk-test/.scratch | `links/symlink_relative_move` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `links/symlink_long_target` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `features/sparse_files` | PASS | 1 GiB logical, 128.00 KiB allocated<br>2 data ranges reported |
| /home/ghadyani/disk-test/.scratch | `features/punch_hole` | PASS | allocated 16.00 MiB -> 8.00 MiB |
| /home/ghadyani/disk-test/.scratch | `features/preallocate` | PASS | allocated after reserving 64 MiB: 64.00 MiB |
| /home/ghadyani/disk-test/.scratch | `features/xattrs` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `features/byte_range_locks` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `features/change_notification` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `features/volume_info` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `concurrency/parallel_files` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `concurrency/disjoint_pwrite` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `concurrency/atomic_append` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `concurrency/exclusive_create_race` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `concurrency/mkdir_race` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `concurrency/rename_race` | PASS |  |
| /home/ghadyani/disk-test/.scratch | `concurrency/readers_during_writes` | PASS | 4918266 reads, 193 torn |

## Feature matrix

| Feature | ext4 |
|---|---|
| Directory fsync | yes |
| Files > 4 GiB | yes |
| mmap shared coherence | yes |
| Direct I/O | yes (align 4096) |
| Anonymous temp files | O_TMPFILE + linkat |
| mtime resolution | 1 ns |
| Timestamps before 1970 | yes |
| Timestamps after 2038 | yes |
| atime updates | strictatime |
| Birth time | yes |
| ctime updated on rename | yes |
| Read-only enforcement | yes |
| POSIX mode bits | yes |
| Block size | 4096 |
| Directory nlink | 2 + subdirs |
| Longest path created | 3925 chars |
| Unicode normalization | none (byte-exact) |
| Special-character names | 23/23 accepted |
| Windows-reserved chars allowed | backslash asterisk question dquote lt gt pipe ctrl-01 colon |
| DOS device names usable | CON nul.txt AUX COM1 LPT1.log |
| Max name: ASCII chars | 255 |
| Max name: 'é' chars | 127 |
| Max name: emoji chars | 63 |
| Case sensitive | yes |
| Case preserving | yes |
| Atomic rename-replace | yes |
| Rename no-replace | yes |
| Rename exchange | yes |
| Rename dir over empty dir | replaces |
| Reflink / block clone | no |
| Copy (std::filesystem) keeps | mode |
| Copy (kernel) keeps | content only |
| Copy (readwrite) keeps | content only |
| Delete open file | POSIX (name freed immediately) |
| Delete read-only file | allowed |
| Delete mapped file | allowed |
| Hard links | yes |
| Hard links per file (tested) | >= 1000 |
| rename(link, same-inode link) | no-op (POSIX) |
| Symbolic links | yes |
| Create through dangling symlink | creates target |
| Sparse files | yes |
| Hole/data query (SEEK_HOLE) | yes |
| Hole punching | yes (deallocates) |
| Preallocation | yes |
| Extended attributes | extended attributes (user.* namespace) |
| Largest xattr value (tested) | 1.00 KiB |
| xattrs per file (tested) | 95 |
| Byte-range locks | yes |
| Change notification | yes (14.7 us) |
| Filesystem type | ext4 |
| Max name length (reported) | 255 |
| Atomic concurrent append | yes |
| Torn 4K reads under concurrent writes | 193 observed |

## Benchmarks

| Target | Benchmark | Params | Throughput | Ops/s | Lat avg | p50 | p99 | p99.9 | Note |
|---|---|---|---|---|---|---|---|---|---|
| /home/ghadyani/disk-test/.scratch | `io/seq_write` | bs=1M buffered T=1 | 2.12 GiB/s | 2.2k | 143.3 us | 134.1 us | 247.2 us | 302.3 us | incl. final fsync 80.95 ms |
| /home/ghadyani/disk-test/.scratch | `io/seq_write` | bs=1M direct T=1 | 2.66 GiB/s | 2.7k | 344.9 us | 338.3 us | 438.8 us | 443.8 us | incl. final fsync 5.02 ms |
| /home/ghadyani/disk-test/.scratch | `io/seq_overwrite` | bs=1M buffered T=1 | 2.34 GiB/s | 2.4k | 87.2 us | 81.3 us | 199.5 us | 211.6 us | incl. final fsync 84.15 ms |
| /home/ghadyani/disk-test/.scratch | `io/seq_overwrite` | bs=1M direct T=1 | 2.42 GiB/s | 2.5k | 382.8 us | 370.3 us | 554.4 us | 562.1 us | incl. final fsync 4.94 ms |
| /home/ghadyani/disk-test/.scratch | `io/seq_read` | bs=1M buffered T=1 | 2.21 GiB/s | 2.3k | 439.8 us | 447.3 us | 601.1 us | 810.9 us | cache dropped before run |
| /home/ghadyani/disk-test/.scratch | `io/seq_read` | bs=1M direct T=1 | 2.85 GiB/s | 2.9k | 341.3 us | 332.9 us | 472.5 us | 512.8 us |  |
| /home/ghadyani/disk-test/.scratch | `io/seq_read_cached` | bs=1M buffered T=1 | 12.90 GiB/s | 13.2k | 74.6 us | 61.4 us | 148.6 us | 169.2 us | warm cache |
| /home/ghadyani/disk-test/.scratch | `io/seq_write_mt` | bs=1M buffered T=4 | 2.22 GiB/s | 2.3k | 206.0 us | 164.9 us | 540.9 us | 617.5 us | incl. final fsync 96.24 ms |
| /home/ghadyani/disk-test/.scratch | `io/seq_write_mt` | bs=1M direct T=4 | 3.03 GiB/s | 3.1k | 1.17 ms | 1.18 ms | 1.31 ms | 1.34 ms | incl. final fsync 6.62 ms |
| /home/ghadyani/disk-test/.scratch | `io/seq_read_mt` | bs=1M buffered T=4 | 3.35 GiB/s | 3.4k | 1.15 ms | 1.16 ms | 1.64 ms | 1.97 ms |  |
| /home/ghadyani/disk-test/.scratch | `io/seq_read_mt` | bs=1M direct T=4 | 3.35 GiB/s | 3.4k | 1.15 ms | 1.16 ms | 1.26 ms | 1.46 ms |  |
| /home/ghadyani/disk-test/.scratch | `io/rand_read` | bs=4K buffered T=1 | 119.32 MiB/s | 30.5k | 32.7 us | 43.1 us | 75.5 us | 211.2 us | page cache warms during run |
| /home/ghadyani/disk-test/.scratch | `io/rand_read` | bs=4K buffered T=4 | 14.31 GiB/s | 3.75M | 994 ns | 553 ns | 1.9 us | 51.9 us | page cache warms during run |
| /home/ghadyani/disk-test/.scratch | `io/rand_read` | bs=4K direct T=1 | 82.45 MiB/s | 21.1k | 47.3 us | 43.6 us | 80.8 us | 263.8 us |  |
| /home/ghadyani/disk-test/.scratch | `io/rand_read` | bs=4K direct T=4 | 346.86 MiB/s | 88.8k | 44.9 us | 42.7 us | 73.8 us | 234.1 us |  |
| /home/ghadyani/disk-test/.scratch | `io/rand_write` | bs=4K buffered T=1 | 4.99 GiB/s | 1.31M | 702 ns | 665 ns | 1.2 us | 3.2 us | incl. final fsync 82.11 ms |
| /home/ghadyani/disk-test/.scratch | `io/rand_write` | bs=4K buffered T=4 | 5.25 GiB/s | 1.38M | 2.7 us | 2.6 us | 4.0 us | 7.5 us | incl. final fsync 82.23 ms |
| /home/ghadyani/disk-test/.scratch | `io/rand_write` | bs=4K direct T=1 | 275.50 MiB/s | 70.5k | 14.1 us | 11.9 us | 35.7 us | 93.4 us | incl. final fsync 5.04 ms |
| /home/ghadyani/disk-test/.scratch | `io/rand_write` | bs=4K direct T=4 | 1.13 GiB/s | 297.0k | 13.3 us | 12.4 us | 23.2 us | 47.7 us | incl. final fsync 4.93 ms |
| /home/ghadyani/disk-test/.scratch | `io/rand_mixed` | bs=4K buffered T=1 r/w=70/30 | 198.81 MiB/s | 50.9k | 19.1 us | 2.0 us | 69.9 us | 161.4 us | incl. final fsync 44.85 ms |
| /home/ghadyani/disk-test/.scratch | `io/rand_mixed` | bs=4K buffered T=4 r/w=70/30 | 14.48 GiB/s | 3.79M | 946 ns | 648 ns | 2.9 us | 38.5 us | incl. final fsync 82.25 ms |
| /home/ghadyani/disk-test/.scratch | `io/rand_mixed` | bs=4K direct T=1 r/w=70/30 | 105.75 MiB/s | 27.1k | 36.8 us | 42.0 us | 73.2 us | 234.4 us | incl. final fsync 4.85 ms |
| /home/ghadyani/disk-test/.scratch | `io/rand_mixed` | bs=4K direct T=4 r/w=70/30 | 423.33 MiB/s | 108.4k | 36.7 us | 39.2 us | 106.6 us | 198.6 us | incl. final fsync 5.08 ms |
| /home/ghadyani/disk-test/.scratch | `io/mmap_read` | window=256M | 1.55 GiB/s | 1.6k | 621.5 us | 626.9 us | 793.5 us | 801.9 us | 1 MiB per op, every cache line touched |
| /home/ghadyani/disk-test/.scratch | `latency/sync_write` | 4K append+fsync |  | 197.4 | 5.07 ms | 5.03 ms | 5.45 ms | 6.26 ms |  |
| /home/ghadyani/disk-test/.scratch | `latency/sync_write` | 4K append+fdatasync |  | 194.3 | 5.15 ms | 5.06 ms | 9.88 ms | 9.91 ms |  |
| /home/ghadyani/disk-test/.scratch | `latency/sync_write` | 4K overwrite+fdatasync |  | 741.9 | 1.35 ms | 1.47 ms | 2.02 ms | 5.30 ms |  |
| /home/ghadyani/disk-test/.scratch | `latency/sync_write` | 4K overwrite O_DSYNC |  | 755.5 | 1.32 ms | 1.47 ms | 1.85 ms | 2.26 ms |  |
| /home/ghadyani/disk-test/.scratch | `latency/sync_write` | 4K overwrite direct+O_DSYNC |  | 271.5 | 3.68 ms | 3.81 ms | 4.08 ms | 8.61 ms |  |
| /home/ghadyani/disk-test/.scratch | `meta/create` | N=2000 |  | 83.4k | 12.0 us | 8.2 us | 34.9 us | 56.3 us |  |
| /home/ghadyani/disk-test/.scratch | `meta/stat` | N=2000 |  | 964.5k | 1.0 us | 948 ns | 1.9 us | 3.5 us |  |
| /home/ghadyani/disk-test/.scratch | `meta/open_close` | N=2000 |  | 828.1k | 1.2 us | 1.2 us | 1.6 us | 3.0 us |  |
| /home/ghadyani/disk-test/.scratch | `meta/readdir` | N=2000 |  | 2.61M |  |  |  |  | full listing avg 767.6 us |
| /home/ghadyani/disk-test/.scratch | `meta/set_times` | N=2000 |  | 734.2k | 1.4 us | 1.3 us | 1.9 us | 3.9 us |  |
| /home/ghadyani/disk-test/.scratch | `meta/rename` | N=2000 |  | 123.0k | 8.1 us | 7.9 us | 12.9 us | 20.6 us |  |
| /home/ghadyani/disk-test/.scratch | `meta/rename_cross_dir` | N=2000 |  | 93.8k | 10.6 us | 10.3 us | 19.1 us | 54.1 us |  |
| /home/ghadyani/disk-test/.scratch | `meta/delete` | N=2000 |  | 334.3k | 3.0 us | 2.8 us | 4.6 us | 16.9 us |  |
| /home/ghadyani/disk-test/.scratch | `meta/create_write_4k` | N=2000 | 425.02 MiB/s | 108.8k | 9.2 us | 8.5 us | 22.2 us | 51.4 us |  |
| /home/ghadyani/disk-test/.scratch | `meta/read_4k` | N=2000 warm | 1.80 GiB/s | 471.9k | 2.1 us | 2.1 us | 2.7 us | 5.0 us |  |
| /home/ghadyani/disk-test/.scratch | `meta/create_write_4k_fsync` | N=2000 | 770.13 KiB/s | 192.5 | 5.19 ms | 5.11 ms | 6.14 ms | 10.39 ms |  |
| /home/ghadyani/disk-test/.scratch | `meta/delete_4k` | N=2000 |  | 130.3k | 7.7 us | 4.5 us | 28.7 us | 60.2 us |  |
| /home/ghadyani/disk-test/.scratch | `meta/mkdir` | N=2000 |  | 82.9k | 12.0 us | 11.5 us | 20.4 us | 85.0 us |  |
| /home/ghadyani/disk-test/.scratch | `meta/rmdir` | N=2000 |  | 179.8k | 5.5 us | 5.3 us | 8.7 us | 10.4 us |  |
| /home/ghadyani/disk-test/.scratch | `meta/hardlink` | N=2000 |  | 120.3k | 8.3 us | 7.8 us | 15.1 us | 50.1 us |  |
| /home/ghadyani/disk-test/.scratch | `meta/symlink` | N=2000 |  | 113.5k | 8.8 us | 8.5 us | 15.3 us | 32.7 us |  |
| /home/ghadyani/disk-test/.scratch | `meta/parallel_create` | N=2000 T=4 same-dir |  | 115.8k | 30.2 us | 24.3 us | 51.4 us | 2.84 ms |  |
| /home/ghadyani/disk-test/.scratch | `meta/parallel_create` | N=2000 T=4 dir-per-thread |  | 249.0k | 12.6 us | 11.8 us | 24.7 us | 59.0 us |  |
| /home/ghadyani/disk-test/.scratch | `meta/tree_mkdirs` | dirs=1000 |  | 122.5k | 8.2 us | 6.7 us | 19.6 us | 29.1 us |  |
| /home/ghadyani/disk-test/.scratch | `meta/tree_walk_stat` | entries=3110 |  | 490.9k |  |  |  |  | warm cache |
| /home/ghadyani/disk-test/.scratch | `ops/copy_file` | 256M read/write 1M | 1.25 GiB/s | 5.0 |  |  |  |  |  |
| /home/ghadyani/disk-test/.scratch | `ops/copy_file` | 256M read/write 64K | 1.15 GiB/s | 4.6 |  |  |  |  |  |
| /home/ghadyani/disk-test/.scratch | `ops/copy_file` | 256M kernel copy | 1.13 GiB/s | 4.5 |  |  |  |  |  |
| /home/ghadyani/disk-test/.scratch | `ops/copy_file` | 256M std::fs::copy_file | 935.07 MiB/s | 3.7 |  |  |  |  |  |
| /home/ghadyani/disk-test/.scratch | `ops/copy_file` | 256M mmap | 942.77 MiB/s | 3.7 |  |  |  |  |  |
| /home/ghadyani/disk-test/.scratch | `ops/copy_file` | 256M read/write 4M direct | 1.53 GiB/s | 6.1 |  |  |  |  |  |
| /home/ghadyani/disk-test/.scratch | `ops/copy_tree` | files=1000 37M std::fs::copy | 914.41 MiB/s | 24.1k |  |  |  |  | incl. syncfs |
| /home/ghadyani/disk-test/.scratch | `ops/copy_tree` | files=1000 37M kernel copy T=4 | 1.07 GiB/s | 28.8k |  |  |  |  | incl. syncfs |
| /home/ghadyani/disk-test/.scratch | `ops/delete_tree` | files=1000 |  | 87.2k |  |  |  |  |  |
| /home/ghadyani/disk-test/.scratch | `ops/delete_large` | 256M unlink |  | 92.6 |  |  |  |  |  |
| /home/ghadyani/disk-test/.scratch | `ops/delete_large` | 256M truncate |  | 48.8 |  |  |  |  |  |
| /home/ghadyani/disk-test/.scratch | `ops/atomic_save` | 16K N=200 |  | 97.3 | 10.27 ms | 10.15 ms | 11.58 ms | 14.91 ms |  |
| /home/ghadyani/disk-test/.scratch | `ops/cross_device` | copy 256M -> tmpfs | 2.02 GiB/s | 8.1 |  |  |  |  |  |
| /home/ghadyani/disk-test/.scratch | `ops/cross_device` | move 256M -> tmpfs | 1.58 GiB/s | 6.3 |  |  |  |  |  |
| /home/ghadyani/disk-test/.scratch | `ops/cross_device` | move tree files=1000 -> tmpfs | 1.50 GiB/s | 40.3k |  |  |  |  |  |
