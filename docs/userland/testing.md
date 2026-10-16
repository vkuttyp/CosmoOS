# Userland: testing

## Overview

| Layer | Mechanism | Command |
|---|---|---|
| System calls through libc | `init --selftest` (kernel self-test `process-user`) | `make test` (self-test builds) |
| Shell and utilities, scripted | `/etc/rc`, run by init, runs `/etc/rc.test`; the script prints `SHTEST: PASS` or `SHTEST: FAIL n` | `make test` (self-test builds; the script is packed only with `SELFTEST=1`) |
| Shell, interactive | `tests/boot/shelltest.py` types commands at the `cosmo$ ` prompt through QEMU's serial stdin and checks the log | `make test`, every build |
| Kill paths | Kernel self-test `process-spawn` kills `init --block` and `init --spin` | `make test` |
| Serial-log markers | `init: CosmoOS userland, pid N`, `CosmoOS userland ready`, `init: rc exited with status 0`, `interactive-ok`, `init: shell exited with status 0`, `[ INFO] init exited with status 0`; self-test builds add `USERTEST: PASS`, `SHTEST: PASS` | `tests/boot/run_boot_test.py` |

## `init --selftest`

Listed in `docs/kernel/process/testing.md` (`process-user`), which also
records what each section costs: the suite times every section and
prints `USERTEST: section <name> <ms> ms` plus a total, so a slow one is
named rather than the whole run (invariant **F13**). In short:
the Phase 7 filesystem checks with the libc directory stream and stdio
on a file; the Phase 8 socket checks through the libc names and
`inet_pton`/`inet_ntop`; the Phase 9 checks (`proc_selftest`): pipes and
`dup`/`dup2`, the console as a character device, `spawn` of `echo` into
a pipe and its status, `sh -c "cd /tmp && pwd && exit 7"` (the child's
cwd and status, the parent's cwd unchanged), a `cat` blocked on a pipe
killed with `SIGKILL` (status 137, `WNOHANG` returned 0 before),
`kill` of a bad pid (`ESRCH`) and signal 0 (`EINVAL`), hostile spawn
requests (closed parent handle `EBADF`, duplicate child slot `EINVAL`,
a non-executable file and a directory `EACCES`, a missing file `ENOENT`,
an empty `argv` `EINVAL`), `waitpid` with no children (`ECHILD`),
`chdir`/`getcwd` with `..` and `.` normalisation, `ENOTDIR`, `ENOENT`,
`ERANGE`, `getppid() == 0`, its own `procinfo` record, `klog_read`,
`sysctl_get` (`kernel.name`, `hw.ncpu`, `sysctl.names`, `ENOENT`,
truncation), `malloc`/`realloc`, `snprintf`, `strtol`, `setenv`/`getenv`.
Since the unix-sockets unit a `unix` section (`unix_selftest`): a
stream pair carries bytes both ways and `SO_PEERCRED` names this
process; a child echoes over the end it was given through the spawn
map and answers a message carrying a file handle with the file's bytes;
`sendmsg` of a handle without TRANSFER is `EPERM` and of a unix socket
`EINVAL`; a listener bound at `/tmp/ux-sock` is a `DT_SOCK` node of mode
0755 that `open` refuses `ENXIO`, a child connects to it and the
accepted socket's `SO_PEERCRED` is the child's pid, `getsockname` is
the path and a second bind `EADDRINUSE`; a uid-1000 child is refused
`EACCES` by the node's mode; a jailed child (`spawnve_in`) reaches
neither the parent's abstract name (`ECONNREFUSED`) nor its path
(`ENOENT`) while an unjailed connect to the abstract name works; the
name outlives the socket (`ECONNREFUSED`) until `unlink` (`ENOENT`);
datagrams by name with the sender's abstract name back through
`cosmo_recvmsg` and `MSG_TRUNC`; `SOCK_SEQPACKET` `ESOCKTNOSUPPORT`; and
`USERBENCH: unix`, a one-byte round trip to a child over a stream pair
against the same over two pipes. Since the file-regions unit an `mmap` section (`mmap_selftest`): a
`MAP_SHARED` mapping of a `/tmp` file is coherent with `read()` and
`write()` in both directions with no `msync` between; a spawned child
maps the same file shared and writes a byte the parent reads (**the
first shared memory between two processes in this system**); a
`MAP_PRIVATE` mapping's written page is a copy (`vm.file_cow_faults`
+1, the file and the shared mapping untouched) while its unwritten page
still shows a later `write()`; the offset; `msync`'s rules (`SYNC|ASYNC`
and an undefined bit `EINVAL`, an unmapped page `ENOMEM`, an unaligned
address `EINVAL`, an anonymous range 0); the native rules a program can
probe (neither or both of `SHARED`/`PRIVATE`, `SHARED|ANONYMOUS`, an
unaligned offset, an undefined bit `EINVAL`; a bad fd `EBADF`; a
directory `ENODEV`); a read-only fd (`MAP_SHARED|PROT_WRITE` `EACCES`,
a private writable mapping fine, `mprotect(PROT_WRITE)` of its shared
mapping `EACCES`, `PROT_NONE` and back allowed); a read/write handle
duplicated with `COSMO_RIGHT_READ` alone (`cosmo_dup_rights`): the same
two refusals, the rights and not only the mode deciding; four children judged by
status -- `mmap-past-end` (a two-page mapping of a five-byte file: the
second page is `SIGBUS`, 135, not zeros), `mmap-truncate` (three pages
mapped and installed, the file reopened `O_TRUNC`, the touch is
`SIGBUS`, not the old bytes), `mmap-as-limit` (`COSMO_RLIMIT_AS` lowered
to a page: a file mapping is `ENOMEM`), `mmap-mem-limit` (a private page read, then
`COSMO_RLIMIT_MEM` set to 0, then written: the copy is refused and the
touch is fatal, 139 -- the check review found missing on the replace
path), `mmap-cycle` (200 map/write/unmap cycles, shared and private;
its exit runs the kernel's `file_pages == 0` check by construction); a
`write()` into a page mapped `PROT_READ|PROT_EXEC` shared runs the
kernel-alias instruction-cache sync (`vm.cache_exec_syncs` +1; a
regression check, TCG cannot show coherence); **the futex keyed by what
the word maps** (the shared-futex unit): a child (`mmap-futex-wait`)
maps the file shared and sleeps on a word in it, the parent counts it
asleep through a requeue of the word onto itself -- a count that
crosses the process boundary only if the key does -- then writes and
wakes it (1 woken, the child exits 0: the first wait across two
processes in this system); the wake before the sleep (0 woken, the
child's wait `EAGAIN`); a private mapping's word at the same offset is
neither counted nor woken by the shared key and is by its own; a thread
asleep on a second shared mapping's word whose mapping is then unmapped
times out cleanly; a waiter on the shared word requeued onto a private
word and woken there, the counts moving with it; private waiters
requeued onto a shared mapping's word and the mapping then unmapped
(they time out, the reference each moved waiter was given outliving it); a
double requeue shared (file A) -> private -> shared (file B) with file A
unmapped, closed and unlinked in between: its one page leaves the cache
(`vm.cache_pages` -1, the exchanged reference having let it go) and the
wake through B finds the waiter; `USERBENCH: futex` (50 000 wakes with
no waiter before any shared mapping exists and again with one, and 500
cross-process round trips through two shared words with
`mmap-futex-pingpong`); on cosmofs (`vda` at `/mnt`), a write through a
shared mapping dirties by one fault, `MS_SYNC` writes exactly that page
(`vm.cache_writebacks` +1), a second `MS_SYNC` writes nothing, a second
write faults again (the PTE was lowered) and the third `MS_SYNC` writes
it again; the bench (`USERBENCH: mmap ...`: `read()` of a cached 2 MiB
file, the first touch of every page through a shared mapping, and the
dirtying write of every page); and at the end, the files gone,
`vm.cache_pages` back at its start.

## `/etc/rc.test`

Straight-line shell (no control flow exists); `FAILS` is set to 1 by
`||` on a command that must succeed or by `&&` on a command that must
fail, and the last line runs `sh -c "exit $FAILS"` to print the verdict.
That line is `A && B || C`, and until the shell's AND-OR lists were made
left-associative it could print only `SHTEST: PASS`: on a failing run
neither branch ran, so the `SHTEST: FAIL n` this table promises was
unreachable and a failure showed up only as a missing marker. The script
now asserts the shape it depends on -- both branches of `false && … || …`
and of `true && … || …`, each checked with a two-term list that means the
same thing under either parse.
What it covers, in order: `mkdir -p` of a nested path; `>` and `>>`;
one-, two- and three-stage pipelines through `cat`; `cat` of the
results (the log shows `hello` and `hello` then `world`); `cp` into a
directory and `cp -r`; `ls` of the copy; `mv`; `rm` of a moved-away
file fails; `rm -r`; `ls` of a removed directory fails; a named pipe
(the named-pipes unit): `mkfifo`, then `sh -c "echo via-fifo > fifo" |
cat fifo` -- the two commands of a pipeline start together and meet on
the name, the inner shell's open waiting for `cat`'s, and the log shows
`via-fifo` (a required marker; the redirection sits inside `sh -c`
because this shell opens a command's redirections before it spawns,
and a trailing `&` runs in the foreground without job control);
`false && ...` and `true || ...`; `false; echo "status $?"` (prints `status 1`);
`2>` capture of `cat`'s error and its display; `sh -c "exit 3"` (prints
`exit 3`); a missing command (prints `notfound 127`); variables
(`X=42; echo "var $X ${X}1 $$"`); `export` visible in a child `sh -c`;
`cd` with `pwd` into `/tmp/shtest`, `dir`, `..`; `ls -l | cat > file`
then `cat` of it; `echo -n`; `ps`; `sysctl kernel.name`; `kill 99999`
fails; the package section (`pkg update`, `install fortune`, `badsig`
and `badsum` refused, `hello=1.0` then `upgrade`, removal in dependency
order; `docs/pkg/testing.md`); `rm -r` of the work directory; `SHTEST:
PASS`.

## Interactive harness (`tests/boot/shelltest.py`)

For each entry of `COMMANDS`, waits until the serial log holds one more
`cosmo$ ` prompt than commands sent, writes the command and `\n` to
QEMU's stdin, then checks the listed patterns against the whole log at
the end:

| Typed | Required in the log |
|---|---|
| `echo interactive-ok` | `^interactive-ok$` |
| `ls /bin` | `^sh$`, `^cat$` |
| `ps` | a running `init` line with one thread, a `ps` line |
| `echo $((` | nothing crashes (the line prints `$((`) |
| `pwd` | `^/$` |
| `cd /tmp && pwd && cd /` | `^/tmp$` |
| `sysctl kernel.name` | `^kernel.name = CosmoOS$` |
| `dmesg` | the `serial: console input on IRQ 4` line |
| `nosuchprogram` | `^sh: nosuchprogram: not found$` |
| `sleep 5` then a bare `0x03` | `\^C` (the terminal's echo); the job exits 130 and the next prompt arrives without waiting out the five seconds |
| `echo after-interrupt-ok` | `^after-interrupt-ok$` (the shell survived the interrupt it sent to the job) |
| `pkg install hello && hello && pkg list` | `^hello, world \(hello 1\.1\)$`, `^hello\s+1\.1\s+prints a greeting$` (`docs/pkg/testing.md`) |
| `exit 0` | the run ends: `init: shell exited with status 0` |

The interrupt is the one entry that is not a line: it is sent as a raw
byte half a second after the command it interrupts, without waiting for
a prompt -- the shell is waiting for the job, not for a line, and that
is the only state in which the byte means anything. It is also the only
entry that does not advance the prompt count.

Failures appear as `shell harness: ...` lines (`no prompt before
command N`, `never sent`, `after 'cmd' missing /pattern/`).

## Measured results (2026-09-05, QEMU TCG, Apple Silicon host)

| Configuration | Result |
|---|---|
| debug, `-smp 4` | `SELFTEST: PASS (61 tests)`, `USERTEST: PASS`, `SHTEST: PASS`, harness complete, about 10 s |
| debug, `-smp 1` | PASS |
| release | PASS (no self-tests; `rc.test` absent; the interactive harness runs) |
| `make test-crash`, `make host-test`, `make analyze`, `make reproducible` | PASS |

## `make test-install` (`tests/boot/install_test.py`)

Roadmap M2's acceptance: install to a blank disk image, reboot from it,
write a file, reboot, read it. Release builds in CI (job `install`, both
architectures); each boot is QEMU through `scripts/qemu-run.sh`, commands
typed at the prompt, every serial log kept in `$(OUT)/test-install/`.

- **install**: the live image with a blank 256 MiB virtio disk; the disk
  is found in `cosmo-install --list` by its size; `cosmo-install DEV`
  must print the `installed on` line and exit 0; a second run without
  `--force` exits 3 with the refusal; the new root is mounted and must
  hold `/sbin/cosmo-install`, the `/etc/rc` line `svc boot`, and the
  mountpoint directories. On the host: both GPT copies valid and equal,
  two partitions of the ESP and CosmoOS-root types, partition 2's GUID
  the one printed, and partition 1 equal to the boot image's ESP in every
  sector (its signed command line says `root=boot-disk`; roadmap M5).

- **reboot**: the installed disk alone (`QEMU_DISKS=boot`: no scratch
  disk, no USB, the boot disk writable), the same release firmware. The
  log must show the kernel's `root: root=boot-disk is DEV` and `boot:
  verified boot, key <id>`, `init: switched to the disk root DEV
  (root=boot-disk)`, the kernel's `root switched to cosmofs`
  with at least one mount moved, `the old root (ramfs) is released`, and
  the installed `/etc/rc`'s `CosmoOS userland ready`; then
  `echo NONCE > /persist.txt && sync` and `/dev/console` listed; on exit,
  `init: disk root synced`.
- **persist**: the same disk booted again: `cat /persist.txt` prints the
  nonce; then `pkg update && pkg install hello && hello && pkg list` exits
  0 with `hello, world (hello 1.1)` and `hello 1.1 prints a greeting`, from
  the repository the installer copied to `/var/lib/pkg/repo` (roadmap M6;
  the install stage also checks the installed `repos.conf` names it and
  that it holds `INDEX`).
- **fallback**: a copy of the live image whose command line says
  `root=vda` (the blank scratch disk on both machines), re-signed with the
  build's key (`--key`, `scripts/mkmanifest.py resign`): the kernel
  resolves it, init says
  `cannot mount vda ... staying on the live root`, and the live shell
  answers.

## `make test-net-login` (`tests/boot/net_login_test.py`)

Roadmap M4's acceptance. Release builds in CI (job `net-login`, both
architectures). The live image is installed as `test-install`'s install
stage does, then the installed disk is booted alone on QEMU user
networking with no fw_cfg address, so the kernel's DHCP client configures
`eth0`; the log must show `dhcp: eth0: bound to 10.0.2.15 ... first
10.0.2.3`.

- **a. DHCP**: the boot's slirp names the machine (`QEMU_NET_HOSTNAME`,
  DHCP option 12); `netcfg show` reports `eth0: bound 10.0.2.15/24 gateway
  10.0.2.2 server 10.0.2.2 ... dns 10.0.2.3 hostname cosmo-s1`, and
  `/etc/resolv.conf` holds `nameserver 10.0.2.3`.
- **b. By address**: the harness serves a fresh random 1 MiB file over
  HTTP on the host's 127.0.0.1 (the guest's 10.0.2.2); `wget` fetches it
  and the guest's `sha256sum` must equal the file's.
- **c. By name**: `nslookup example.com` exits 0 with an `Address:` line,
  and `wget -O - http://example.com/` returns a body containing `Example
  Domain` -- up to three attempts, since this depends on the runner's
  internet access through slirp's resolver (10.0.2.3, the host's).
- **Console job control** (M4 PR 2), the same boot, ash on the console:
  `sleep 100 &` then `jobs` lists it Running, `kill %1` and `wait`; `sleep
  30` stopped by `^Z` (`Stopped`), `jobs`, `bg` (`Running`), `kill %1`;
  `readlink /proc/self/fd/0` is `/dev/console`.
- **d. ssh** (M4 PR 3): the boot forwards a free host port to the guest's
  22 (`QEMU_NET_HOSTFWD`). The harness makes an Ed25519 key and a
  3072-bit RSA key, types both public halves into
  `/root/.ssh/authorized_keys` at the console, and waits
  (120 s at most) for dropbear to answer -- it makes its host key at this
  first boot. The host's OpenSSH client (`BatchMode`, `IdentitiesOnly`, a
  known_hosts of the run's own, `accept-new`) runs `uname -a; echo ok`,
  and with the RSA key `echo rsa-ok`; then `ssh -tt` under a host pty of 37 x 101 types `tty` (must print
  `/dev/pts/N`), `stty size` (must print `37 101`), `sleep 100 &`, `jobs`
  (Running), `kill %1`, `wait` and a marker.
- **g. Network inspection** (shell completeness S1), at the end of the
  same boot: `ip a` shows `lo` and `eth0` (`<BROADCAST,MULTICAST,UP,LOWER_UP>
  mtu 1500 state UP`, its MAC, `inet 10.0.2.15/24 brd 10.0.2.255 scope
  global dynamic eth0` with the lease's lifetime, the link-local address);
  `ip -4 a` prints no `link/` or `inet6` line; `ip r` the default via
  10.0.2.2 `proto dhcp` and the subnet; `ip neigh` the gateway
  `REACHABLE`; `ip addr add` is refused (exit 2). BusyBox: `ifconfig`
  (eth0's MAC, address, broadcast, mask, flags and a non-zero RX count,
  and `lo`), `route -n` (the default `UG` and the subnet `U`), `netstat
  -tuln` (dropbear on `0.0.0.0:22` and `:::22`, no "bogus data"), `arp -a`
  (the gateway at the MAC `ip neigh` gave), `hostname`, `hostname -s` and
  `/etc/hostname` (`cosmo-s1`). `priv -d net -- ifconfig eth0 down` must
  fail with `SIOCSIFFLAGS: Operation not permitted` and leave eth0 up;
  `ifconfig eth0 down` as root must show `state DOWN` in `ip link` and no
  `UP` in `ifconfig eth0`; `ifconfig eth0 up` must bring it back, and a
  ping of the gateway must answer. Then each other setter on `eth1` (the
  second NIC, otherwise unused): address, netmask, broadcast, MTU and MAC
  each fail with `Operation not permitted` with `PRIV_NET` dropped and
  leave `eth1` as it was; as root `ifconfig eth1 10.0.3.15 netmask
  255.255.255.0` shows in `ip a`, `ip r` (the subnet; the default still
  through eth0) and `ifconfig`; the broadcast, MTU and MAC it has are
  accepted, and `broadcast 10.0.3.1` (EINVAL), `mtu 1400` and another MAC
  (EOPNOTSUPP) refused, its MAC and MTU unchanged.
- **h. Process tools** (shell completeness S3), at the end of the same
  boot: `echo $$ $PPID` gives the console shell's pid and init's (its
  parent), and `svc status dropbear` the ssh server's supervisor; `ps -o
  pid,ppid,stat,comm` must list init with that pid, parent 0 and state
  `S`, `dropbear` as the supervisor's child in `S`, the shell with init as
  its parent in `S`, and `ps` itself in `R`. Every pid it is checked
  against is known without `/proc`.
- **i. Ownership** (shell completeness S4), at the end of the same boot:
  `/root/s4own` chowned to 1000:1000 and a link to it, `/root/s4lnk`,
  chowned with `-h` to 1001:1001, then `sync`; `stat` must show both.
- **e. Reboot**: the installed disk boots again, its slirp naming no
  host; a login with `StrictHostKeyChecking=yes` against the first boot's
  known_hosts must pass, so the host key was kept, `hostname` must
  still print `cosmo-s1` (`/etc/rc` applied `/etc/hostname`), and `stat`
  must still show i's owners on cosmofs (the file 1000:1000, the link
  1001:1001).

## `make test-busybox` (`tests/boot/busybox_test.py`)

Roadmap M3's acceptance: BusyBox ash is the shell. One boot of the image
(release builds in CI, job `busybox`, both architectures); at the
console, `/bin/sh` runs two scripts from `/boot/tests/busybox`:

- **`ash.sh`, the scripted test**: 165 checks, each `BBTEST: ok NAME` or
  `BBTEST: FAIL NAME: want [..] got [..]`, ending `BBTEST: PASS`. The
  shell's features: a pipeline, `>`/`>>`/`<`, `2>` and `2>&1`, command
  substitution nested and with backquotes, a subshell's variables, `$?`
  after `false`, `true` and `sh -c 'exit 7'`, a background job and
  `wait` (its status, and a backgrounded subshell's output), a `#!`
  script with an argument, a command not found (127) and a file without
  execute permission (126), arithmetic, `for`, `case`, a function. Every
  applet of `ports/busybox/applets` at least once (`echo`, `printf`,
  `test`, `[`, `true`, `false` and `pwd` as `/bin/<name>`, since ash has
  them built in): `mkdir -p`, `rmdir`, `cp`, `mv`, `rm`, `cat`, `ln -s`,
  `readlink`, `readlink -f`, `chmod` read back by `stat -c %a`, `touch`
  (create, and `-t` read back by `date -r`), `ls`, `env`, `sleep` (at
  least a second by `date +%s`), `date -u -d @0`, `seq`, `head`, `tail`,
  `wc`, `sort` and `sort -n`, `uniq`, `cut`, `tr`, `grep -c`, `sed`,
  `awk` (integers and `%.2f`), `find -type f`, `xargs -n 1`, `tar` and
  `tar -z`, `gzip`, `gunzip`, `diff` (both statuses), `basename`,
  `dirname`, `expr`, `tee`, `stat -c %s`, `uname -s` (Linux), `id -u`,
  `sha256sum` of `abc`, `which ls`. Then v1.0.2's everyday commands:
  `md5sum` of `abc`, `dd` of three blocks, `cmp` (both statuses),
  `hexdump -C`, `du -s`, `whoami` (root, from the image's `/etc/passwd`),
  `clear`, `less` writing to a pipe, `uptime`'s load averages, `free`,
  `df /`, the `/proc` files they read (`mounts`, `<pid>/cmdline`,
  `<pid>/stat`), `top -b -n 1` (its memory and load lines and its own
  command line), and `vi` driven by keystrokes from a pipe. Then shell
  completeness S2's applets, each with known input and output: `more` and
  `nl` and `tac` on three lines, `od -b`, `strings` of a binary file, `rev`,
  `yes | head`, `cal 2 2024`'s title, `time -p`'s `real` line, `timeout 1
  sleep 10` (143), `watch` under `timeout`, `nohup`, `install -m 0640`,
  `realpath` through `..`, `split -l 4` of ten lines (4 4 2), `fold -w 3`,
  `paste`, `comm -12`, `cksum` of a known line, `base64` both ways, `unxz`
  and `xz -dc` of a fixture made on the host (BusyBox's `xz` decompresses
  only), `unzip` of a fixture, `bzip2`/`bunzip2` and `gzip`/`zcat` round
  trips, `mktemp`, `setsid` (the child leads its session); `pgrep -f` and
  `pidof` find a background `sleep`, `pkill -f` and `killall` end one (its
  status 143); and `logger -t TAG` reaches the kernel's log through
  `syslogd` (`dmesg` shows `syslog: [user.notice] TAG: ...`). Then shell
  completeness S3's process tools: `ps -o pid,stat,vsz,rss` lists a
  background `sleep` by its pid, `S`, with non-zero VSZ and RSS, and `ps -o
  pid,ruser` its user (`root`, from `status`'s `Uid:`); `ps` lists init as
  the console shell's parent in `S` and the script's own shell in `S`;
  `top -bn1`'s line for the `sleep` has a non-zero VSZ; its `status` has
  non-zero `VmSize` and `VmRSS` and `State:\tS (sleeping)`; `free`'s total
  is `sysctl`'s pages times the page size, and its free figure within 2
  MiB of `sysctl vm.pages_free` read just before (the two readings are
  printed as a `BBTEST: note`); `uptime` says `up`; `/bin/kill -l 9` is
  `KILL` and `kill -l` lists `HUP`; `env S3TAG=s3 cat /proc/self/environ`
  holds the variable, `/proc/self/exe` of `readlink` is `/bin/busybox`,
  `cwd` is `/tmp` after `cd /tmp`, `root` is `/`; `priv -u 1000 -- cat
  /proc/1/environ` says `No such file` (P1); a uid-1000 `sleep` (under
  `priv -u 1000`)'s `environ` is read as root with `sys_admin` dropped
  (`PRIV_PROC` is the rule) and is empty with `proc` dropped too; `/proc/version`'s third word
  is `uname -r`, and `/proc/cpuinfo` has `hw.ncpu` `processor` lines.
  Then shell completeness S4's ownership: `chown 1000:1000` and `chgrp 0`
  read back by `stat -c '%u %g'`; `chown -h` changes a link and not its
  target, and `chown` through a link changes the target; `chown 0` of a
  6755 file leaves 755 (the set-id bits cleared); `chown` is `Operation
  not permitted` to uid 1000 and to root with `sys_admin` dropped, and the
  file is unchanged; with a `tester` (uid 1000) added to `/etc/passwd` and
  `/etc/group` for the test, `su -c 'id -u'` is 0, `su -c ... tester`
  runs as 1000:1000, and with `cred` dropped `su` to `tester` is `not
  permitted`.
- **ash's history** (shell completeness S2): `sh` at the console, a
  command unique to the run, `exit`; `/root/.ash_history` must hold it,
  and a second `sh`'s `history` must list it.
- **`suite.sh`, BusyBox's testsuite**: the files decision 8 names -- cut,
  sed, grep, tr, sort, uniq, head, tail, expr, seq, basename, dirname, wc,
  xargs, tar -- each through runtest, from a copy of the testsuite (a
  reproducible `testsuite.tgz` of BusyBox's own directory) under `/tmp`
  with the binary and `.config` beside it. Every
  case must PASS, be SKIPPED or UNTESTED by runtest itself (a feature
  the configuration leaves out), or be one of the exclusions below; each
  file must pass at least one case.

| File | Case | Why it is excluded |
|---|---|---|
| sed | sed embedded NUL | fails the same way under Linux (arm64 container, this binary and testsuite): this build's behaviour, not the kernel's |
| sed | sed NUL in command | the same |
| sed | sed nonexistent label | the same |
| tar | tar Symlink attack: create symlink and then write through it | needs `uudecode`, not an M3 applet |
| tar | tar hardlinks and repeated files | makes a hard link; hard links are outside M3 |
| tar | tar hardlinks mode | makes a hard link |
| tar | tar symlinks mode | makes a hard link (`ln` without `-s`) in its setup |
| tar | tar --overwrite | makes a hard link |
| tar | tar Symlinks and hardlinks coexist | makes a hard link |

`--verbose` runs runtest with `-v`, so the log holds each failing
case's commands and diff.

The installed system's console is ash too: `make test-install`'s reboot
and persist stages type their commands at it (`/etc/console-shell`).

## Gaps and planned tests

- No test of a redirected builtin (`pwd > file`), of `sh file args`
  with `$1`, of `set -e`, or of `.`/`source`.
- No test types an editing key (`^U`, backspace) at the interactive
  prompt; the tty self-test covers them.
- No test of `cp`/`mv` across mounts (`EXDEV` path), of `mount`/`umount`
  from the shell, or of `kill` of a live process from the shell.
- `ls -l` output is checked only for presence, not for field values.
