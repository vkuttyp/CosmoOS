# The Parallels-test residue (roadmap 1.1 P3) — 2026-10-19

The project owner's task of 2026-10-19, its PR 2: the four small defects
the post-1.0 Parallels test found (inventory section 18), each with a
failing test and a probe. Base: main `f7107c2c` (v1.0.6). Branch
`p3-residue`.

## /dev/zero's 64 KiB read

`syscall_obj_read` made one call to the object's `read`, bounded by the
64 KiB bounce buffer, and returned what it gave: `dd if=/dev/zero bs=1M
count=4` copied four partial records, 256 KiB. POSIX allows the short
read, but Linux fills a read of a regular file or of `/dev/zero`, and
programs count on it.

A `kobject_io_type` may now fill a large read itself (`read_filled`):
`syscall_obj_read` hands a read larger than the bounce to it with the
bounce and a callback that copies a piece out, and the object reads a
bounce at a time until the request is met, a piece comes back short (end
of file), or the copy faults. A file's (`file_obj_read_filled`,
`kernel-services/vfs/vfs.c`) fills for a regular file and for a character
device whose `chrdev_ops.fills` is set; `/dev/zero` sets it (`memdev.c`).
A pipe, socket, terminal or other device still returns what one call
gives.

It is one read. The first version looped in `syscall_obj_read`, a call
per bounce, and #413's review (Codex, Qodo) showed what that lost: each
call took and released the open file's lock, so a second reader or a
seek on the same open file could land between two pieces and the read
return bytes from two places, and a fault on a later piece returned
`-EFAULT` although the earlier pieces had been copied and the position
had moved past them. A regular file now holds `f->lock`, its position's
lock, across every piece and its copy, as Linux holds `f_pos_lock` (the
order is `f->lock` before `vn->lock`, and no fault path takes an open
file's lock), and moves the position by what was copied: a fault after
some pieces returns those bytes, as Linux does, and one before any is
`-EFAULT` with the position unchanged.

## wget's bad-certificate message

The patched `wget` (`ports/busybox/wget-ssl-client.patch`) runs
`ssl_client` and reads the response through it. When `ssl_client`
refused the certificate it printed mbedTLS's reason and exited; `wget`
then read end of file and died with `bb_simple_perror_msg_and_die("error
getting response")`, whose errno was whatever the last failing call left
(after a name lookup, the resolver's `EAGAIN`, so "Resource temporarily
unavailable" on Parallels; QEMU's slirp names the host by address and
left none). `wget` now records `ssl_client`'s pid and, when the response
read finds end of file, waits for it: a non-zero exit ends `wget` quietly
(`xfunc_die`, exit 1), `ssl_client`'s reason the last line. Every new
connection (`open_socket`) clears the pid first, so an https URL
redirected to http never waits for the old connection's helper (#413's
review, Qodo).

## sftp's link count: not a defect

The owner's decision (2026-10-19): record it and amend the acceptance.
An OpenSSH 9.1+ `sftp` client renders `ls -l` itself when the server
offers `users-groups-by-id@openssh.com`, and SFTP v3 attributes carry no
link count, so it prints `?` (`sftp-common.c` `ls_file` with `remote`
set). Linux with OpenSSH's own server shows the same. The server's long
name has `lstat`'s `st_nlink`. `test-net-login` now accepts the client's
`?` or `1` for the file sftp wrote and requires `ssh ... stat -c
'nlink=%h'` on that same file (renamed, the same inode) to print
`nlink=1`; a second sftp batch then removes it and its directory. On both architectures the host's
client printed `?`.

## The installer's free count

Not the installer's: every mount of a cosmofs after a clean unmount
warned `free block count N differs from the superblock's N-4..7`. The
superblock's count is the bitmap's as its commit wrote it; that commit's
released blocks are freed afterwards, by the next mount's free-log replay
(the bitmap bits cleared once the root is durable). `load_bitmap`
compared the two after `freelog_replay`, so it always saw the last
commit's frees. It compares before the replay
(`cosmofs_core.c`).

## Tests

- `test-busybox` (`tests/busybox/ash.sh`): `dd-zero-1m` requires
  `dd if=/dev/zero bs=1M count=4` to report `4+0 4+0 4194304`, and
  `dd-file-1m` the same of a 4 MiB regular file.
- `test-net-login`: the refusal of the installed bundle requires
  `https-refused=1` and no line starting `wget: ` after `ssl_client`'s
  reason; sftp's listing and the server's `stat` as above.
- `test-install`: no free-count warning at the installed root's first
  mount, nor in either installed boot.
- The self-test `file-read-filled` (every debug boot): a regular file
  read from offset 100 in two-page pieces is contiguous, each piece
  handed over with the open file's lock held, the position at the end;
  a fault at the second piece returns the first piece and leaves the
  position after it, a fault at the first is `-EFAULT` with the position
  unchanged; `/dev/zero` fills three pieces of zeros; `/dev/null` reads
  one call at a time (`-EOPNOTSUPP` from `read_filled`).
- `tools/p3-residue-probe.py --defect zero|wget|freecount [--old] [--arch
  A]`: `--old` reverse-applies the fix commit's sources (found by
  subject) in a throwaway worktree and runs the target in a release
  build; `wget` builds BusyBox from that tree. A fixed run passes only
  when the target exits 0 and prints its PASS; an old one only when the
  target exits non-zero with the named check's failure.

| Probe | x86-64 | AArch64 |
|---|---|---|
| `zero --old` | PASS: `dd-zero-1m: want [4+0 4+0 4194304] got [0+4 0+4 262144]` | PASS: the same |
| `zero` | PASS: test-busybox PASS | PASS |
| `wget --old` | PASS: `added 'wget: error getting response' after ssl_client's reason` | PASS: the same |
| `wget` | PASS: test-net-login PASS | PASS |
| `freecount --old` | PASS: `free block count 46336 differs from the superblock's 46332` | PASS: `46302` / `46298` |
| `freecount` | PASS: test-install PASS | PASS |

All twelve at `9276c3f4`.

## Validation

The device-lifecycle matrix at `9276c3f4` (on `f7107c2c`, before the
rebase onto #412's merge `32a89711`, which had no conflict in code), both
architectures, every step exit 0 (seconds):

| Step | x86-64 | AArch64 |
|---|---|---|
| `host-test`, `fuzz`, `analyze` | 82, 34, 211 | 82, 35, 210 |
| debug `test` at `QEMU_SMP=1`, `2`, `4` (network harness in each) | 244, 149, 159 | 247, 163, 168 |
| `test-smp2`, `test-chaos`, `test-harness-retry` | 158, 188, 191 | 152, 186, 195 |
| `BUILD=release all test` | 20 | 23 |
| `BUILD=release test-busybox`, `test-install`, `test-net-login` | 49, 28, 86 | 60, 44, 96 |

After #413's review (`24fb1455`, `c19c613c`: `read_filled`, the
connection's helper pid, sftp's own file, the probe's exit status and its
two-commit `--old`), on both architectures: `host-test`, `analyze`, debug
`make test` (`file-read-filled ... ok`), and `BUILD=release` `all`,
`test-busybox`, `test-net-login` (the server's `stat` of the file sftp
wrote: 1) and `test-install`, every one exit 0; and the `zero` and `wget`
probes again in both modes, all eight PASS (the `--old` runs reversing
the review commit, then the fix).
