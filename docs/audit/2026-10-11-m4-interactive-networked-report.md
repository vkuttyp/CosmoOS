# Roadmap M4: interactive and networked — 2026-10-11

Milestone M4 of the [1.0 roadmap](../roadmap-1.0.md): from the installed
system, BusyBox `wget` fetches a file by hostname through QEMU user
networking; ssh login from the host with static dropbear; both
architectures, in CI. Base: main `9eb42796` (M1-M3 done, CI green).
Decisions 0-6 of the task are the owner's and were not reopened; this
report records how each was built.

## Plan as worked

| PR | Content |
|---|---|
| 1 | Decision 0 (fetch-pinned retries and a second URL); the DHCP client and the lease in netctl; `netcfg` and `/etc/resolv.conf`; BusyBox `wget`, `nslookup`, `stty`, `tty`; `make test-net-login` with acceptance a-c and its CI job. |
| 2 | PTYs, sessions, controlling terminals and job control (decisions 3-4); console job control. |
| 3 | dropbear (decision 6); acceptance d-e; M4 ticked. |

## PR 1 (as built)

| Area | Change | Files |
|---|---|---|
| Pinned sources | `fetch-pinned.py OUTPUT SHA256 URL...`: the URLs in order, three attempts each with doubling backoff (`COSMO_FETCH_ATTEMPTS`, `COSMO_FETCH_BACKOFF`), the fetch fails only when every URL has, and the error names each URL with its last failure. The digest check is unchanged. Each archive's second URL is the copy in this repository's [`pinned-sources`](https://github.com/vkuttyp/CosmoOS/releases/tag/pinned-sources) release (BusyBox 1.37.0, musl 1.2.5, compiler-rt 19.1.7, digests checked before upload). Host test without network: `tests/host/test_fetch_pinned.py`. | `scripts/fetch-pinned.py`, `ports/busybox/busybox.mk`, `tests/host/` |
| DHCP client | RFC 2131 on every interface a NIC driver marks `NETIF_AUTOCONF` (virtio-net, e1000e) that has no fw_cfg `opt/cosmo/ipv4`: SELECTING, REQUESTING, BOUND, RENEWING (unicast REQUEST to the server at T1), REBINDING (broadcast at T2), the address removed at the lease's end or on a NAK (N30). Options 1, 3, 6 (three servers), 51, 54, 58/59. Frames built directly (source 0.0.0.0, limited broadcast); replies claimed in `ipv4_input` before the address checks. `dhcp_boot_wait` (10 s at most) after the boot modules. The hard-coded `10.0.2.15` default is gone; fw_cfg stays the override. | `kernel-services/network/dhcp.c`, `kernel/include/kernel/net/dhcp.h`, `netif.c`, `ipv4.c`, `main.c`, the two drivers |
| Lease in netctl | Snapshot version 7: a lease section after the flow section (`struct cosmo_netctl_lease`: interface, state, address, mask, router, server, DNS servers, lease/T1/T2 and the time left to each). | `uapi/cosmo/netctl.h`, `tap.c` |
| DNS for user space | `netcfg` (native, `/sbin`): waits for a bound lease, writes `/etc/resolv.conf` (renamed into place); `netcfg show`. `/etc/rc` runs it. musl's resolver and BusyBox's `nslookup` read the file. | `userland/system/netcfg.c`, `userland/etc/rc` |
| BusyBox | `wget`, `nslookup`, `stty`, `tty` (defconfig features, `nslookup` "big"; `wget` speaks http only: BusyBox's own TLS checks no certificate, so https and TLS are off). | `ports/busybox/` |
| Tests | `net-dhcp-client` self-test; the client in `fuzz_dhcp_dns` (records 6 and 7, seed 7 driving SELECTING through REBINDING, the lease's end and a NAK; oracle: an address exactly while a lease is held); `make test-net-login` (a-c). | `nettest.c`, `tests/fuzz/`, `tests/boot/net_login_test.py`, `Makefile`, CI |

Design: `docs/kernel-services/network/design.md`, "The DHCP client";
api.md, "The DHCP client"; invariant N30; `docs/userland/design.md`,
"netcfg"; `docs/userland/testing.md`, "make test-net-login".

### Two defects found on the way, both in the new code

- **The receive checksum test was inverted.** The first boot's client
  ignored every OFFER: a capture (`QEMU_PCAP`) showed slirp answering each
  DISCOVER with a correct OFFER for the right `xid` and MAC. `cksum_fold`
  returns the complemented sum, so a valid datagram verifies to 0, and the
  client compared against `0xffff`. `nettest_l4_ok` had the convention
  already.
- **A re-arm of a pending timer.** An input that re-armed the client's
  timer while it was pending panicked (`timer_start: timer ... is already
  pending`, netrx/1, the client lock held): the timer API requires a cancel
  first. `arm` now cancels; a callback already past its start only queues
  the work, which finds nothing due.

Both were in the first build of the feature, so they are not defect fixes
in the probe sense; `net-dhcp-client` covers both paths (every reply is
checksummed by the fixture, and inputs re-arm a pending timer in steps 3-8).

### A defect PR 1's CI exposed: trap-async-inject depended on its CPU

PR 1's first CI run failed the aarch64 entropy job's second boot
(neoverse-v1, run 38119192127): `trap-async-inject ... FAIL: EL2 refused
the virtual SError`. Nothing in PR 1 touches EL2. The test asks EL2 for a
virtual SError with a bare call; EL2 answers that only through the
switch's vectors, which a CPU has once the run loop (or a TLB
invalidation) readied it -- elsewhere the loader's stub refuses (S25,
`kernel/arch/aarch64/hv_el2.c`). So the test passed where it happened to
run on a CPU a guest had used, and the DHCP boot wait moved the
self-tests' timing. It blocks PR 1 (a red CI run) and would turn main red,
so it is fixed here (AGENTS.md, "Scope rules"):

- **Proof** (`de30611d`): the test pins its CPU and hands EL2 back to the
  stub there first; it then fails on every boot of a CPU with FEAT_RAS --
  `test-guard` (cortex-a76), local, the CI symptom exactly.
- **Fix** (`c16df400`): the test readies EL2 on that CPU
  (`aarch64_el2_ready_here`, the run loop's `el2_ready_here`).
- **Probe**: `tools/async-inject-probe.py --arch aarch64 [--old]`, on
  cortex-a76: `--old` PROBE: PASS (the test fails with "EL2 refused the
  virtual SError", the only failure), fixed PROBE: PASS ("a corrected
  SError was taken at EL1 and execution continued"). AArch64 only: x86-64
  has no EL2, and its half of the test (vector 18 has a handler) is
  unaffected.

### PR 1 review

Qodo, Codex and CosmoReview raised 15 findings on PR #351; 13 were fixed in
`18561ad3` (the client's slot lifetime under its own lock, the server identifier
required and matched for ACK/NAK, the lease counted from the REQUEST,
nanosecond T1/T2 for short leases, only datagrams for this host claimed,
`netif_set_ipv4`'s publish order, `secs_since` underflow, netcfg's wait and
path checks, fetch-pinned's bounds, wget without unverified https), and two
are deferred with inventory lines (resolv.conf refresh on a lease change, a
consistent snapshot of an interface's IPv4 tuple). Re-validated: debug
`test`, `analyze` and `test-net-login` on both architectures, `host-test`,
`fuzz`.

### Test fixtures no longer receive 10.0.2.15

Before this PR, every non-loopback interface registered without an address
got `10.0.2.15/24` from `netif_autoconfig`, self-test fixtures included.
Only `NETIF_AUTOCONF` interfaces are configured now; the fixtures set
their own addresses or none. The full debug suite passed without change.

### The revert and the re-land

PR 1 merged as #351; main's aarch64 boot then failed `net-bench`'s 8 s
budget twice (run 38127318850: 8673 ms and 8347 ms, every assertion
passing), and #351 was reverted (#356). The owner's plan then instrumented
`net-bench` (#363, Step 1), ran it with #351 re-applied on CI (#362, Step
2: no retransmission timeouts or drops in the slow rounds; UDP, which
retransmits nothing, the slowest phase), and restated it as fixed 300 ms
windows with per-flow integrity checks (#363, Step 3;
`docs/audit/2026-10-11-net-bench-restate-report.md`). The harness gained
one retry of the firmware-handover stall (#364, Step 4), which had hit
`test-net-login`'s install boot. PR 1 then re-landed unchanged from #351
(a revert of the revert, `17e5bab3`) with these records updated; the
revert and the re-land count once among M4's PRs.

Re-land validation (local, one sequential run of every item, on main
`0b76962c` plus the re-land): `host-test`, `analyze`, debug `test` at
`QEMU_SMP=1/2/4`, `test-smp2`, `test-chaos`, `test-harness-retry`, release
`test` and `test-net-login` PASS on both architectures (`test-net-login`
14.7 s x86-64, 21.4 s AArch64; debug boots 133.8-161.6 s); `fuzz` PASS.

### Acceptance a-c (local, macOS host, QEMU TCG, release builds)

| Step | x86-64 | AArch64 |
|---|---|---|
| a. DHCP | `eth0 10.0.2.15/24 gateway 10.0.2.2 dns 10.0.2.3`, lease 86400 s; `resolv.conf` names 10.0.2.3 | the same |
| b. by address | 1 MiB in 0.2 s, SHA-256 matches | 1 MiB in 0.2 s, SHA-256 matches |
| c. by name | `nslookup example.com` -> 172.66.147.243; `Example Domain` on attempt 1 | the same, attempt 1 |
| whole test | PASS in 14.0 s | PASS in 20.9 s |

### PR 1 validation (local, macOS host, QEMU TCG; one sequential run of every item)

| Item | x86-64 | AArch64 |
|---|---|---|
| `host-test` (with `test_fetch_pinned.py`) | pass | pass |
| `fuzz` (`FUZZ_RUNS=50000`; `fuzz_dhcp_dns` 8 seeds incl. the client's) | pass (host binaries, one run) | -- |
| `analyze` | clean | clean |
| debug `test`, `QEMU_SMP=1` | PASS 141.1 s | PASS 144.9 s |
| debug `test`, `QEMU_SMP=2` | PASS 151.4 s | PASS 142.6 s |
| debug `test`, `QEMU_SMP=4` | PASS 175.0 s | PASS 154.0 s |
| `test-smp2` | PASS 144.4 s | PASS 142.4 s |
| `test-chaos` | PASS 174.8 s | PASS 158.9 s |
| `test-harness-retry` | PASS 147.0 s | PASS 147.5 s |
| release build and `test` | PASS 17.8 s | PASS 20.2 s |
| `test-net-login` (release) | first run: the live boot stopped inside OVMF before its first line (240 s; the fourth sighting in `docs/testing/flakes.md`, "firmware handover"); three reruns PASS 14.9, 14.3, 14.2 s | PASS 25.7 s |

Every debug boot ran the network harness with `eth0` and `eth1` configured
by DHCP (`dhcp: 2 interface(s) bound`) and `net-dhcp-client` passing.

After the trap-async-inject fix (AArch64 only, so re-run there): debug
`test` PASS 144.3 s; `test-guard` (cortex-a76) PASS 144.8 s and
`test-entropy` PASS 156.5 s / 146.4 s, both RAS boots running the test from
a cold CPU (`trap-async-inject ... ok`); the probe as above.

## PR 2 (as built)

| Area | Change | Files |
|---|---|---|
| Pseudo-terminals | `/dev/ptmx` clones a pair; the slave is `/dev/pts/N` on the pts filesystem (a ramfs instance mounted at `/dev/pts`, created and removed by the pty code). The slave is a `struct tty` with the console's line discipline: its input is what the master writes (`tty_input`), its output and echo go into the master's 8 KiB queue, with `OPOST` turning a newline into CR LF. A new slave is locked until `TIOCSPTLCK 0`. Closing the master hangs the slave up (`tty_hangup`: SIGHUP and SIGCONT to the session's foreground group and its leader, reads end of file, writes `-EIO`) and removes its name; the master's read is `-EIO` once a slave was opened and every open of it has closed. A pair lives while its master, a slave open or a `tty_get` holds it. | `kernel/tty/pty.c`, `kernel/include/kernel/pty.h` |
| Many terminals | `struct tty` gains output ops (`echo`, `write`, `get`, `put`), a window size, a hung-up flag and a registry; `tty_controlling(sid)` finds a session's terminal, so `/dev/tty`, the release at a leader's exit and the one-terminal-per-session rule (T17) work for every terminal. Lookups are referenced. | `kernel/tty/tty.c`, `ttydev.c`, `tty.h` |
| Controlling terminals | `TIOCSCTTY` (`tty_set_controlling`), opening a slave without `O_NOCTTY` (`COSMO_O_NOCTTY` new), `TIOCNOTTY`, `TIOCGSID`; `COSMO_SPAWN_SETSID` (`spawnve_session`): the child leads a new session and takes the terminal on its handle 0 before its first instruction -- init starts the console shell so, which gives BusyBox ash job control on the console. | `tty.c`, `process.c`, `spawn.c`, `native.c`, `libc`, `init.c` |
| Job control | `TOSTOP` and `OPOST` are modes; `tty_write` (every write to a terminal: the console object, `/dev/console`, `/dev/tty`, a slave) stops a background writer with SIGTTOU when `TOSTOP` is set (T20); `TIOCSWINSZ` raises SIGWINCH on the foreground group. SIGTTIN, stop and continue, and `wait4`'s `WUNTRACED`/`WCONTINUED` were already there and are exercised. ash's `ASH_JOB_CONTROL` is on. | `tty.c`, `ports/busybox` |
| Linux ABI | `ioctl`: `TCSETS*` (SETSF flushes), `TIOCSWINSZ`, `TIOCSCTTY`, `TIOCNOTTY`, `TIOC[GS]PGRP`, `TIOCGSID`, `TIOCGPTN`, `TIOC[GS]PTLCK`, `FIONREAD`, `TIOCOUTQ`, `TCFLSH`, `TCSBRK`, `TCXONC`; termios carries `ONLCR`, `TOSTOP`, `c_cflag` (CS8, CREAD, B38400) and Linux's default control characters; `O_NOCTTY` reaches the VFS. | `compat/linux` |
| `/proc/<pid>/fd` | One link per open handle: a terminal by its path, a directory by its path, else `file:[ino]` or `<kind>:[address]`; the process's own user and the privileged only. musl's `ttyname` reads it, so `tty` names `/dev/pts/N`. | `procfs.c` |
| switch_root | A mount nested in a top-level mount of the old root (`/dev/pts` in `/dev`) moves with it; it was refused (`-EBUSY`), which the installed boot met first. | `vfs.c` |
| Tests | `tty-pty` (a pair's life from the kernel side); `lxpty` (musl's pty path, `/proc/self/fd` naming the slave with the same inode, then a session: ^C, ^Z with `WUNTRACED`, SIGCONT with `WCONTINUED`, SIGTTIN, TOSTOP's SIGTTOU and its absence, SIGWINCH, the hangup's SIGHUP); `test-net-login`'s console stage (ash: `&`, `jobs`, `kill %1`, ^Z, `bg`; `/proc/self/fd/0`). | `ttytest.c`, `tests/linux/lxpty.c`, `tests/boot/net_login_test.py` |

Design: `docs/kernel/tty/design.md`, "Pseudo-terminals" and "Many
terminals"; invariants T17-T20; `docs/compat/linux/design.md`, `ioctl`;
`docs/kernel/process/api.md` (`COSMO_SPAWN_SETSID`); procfs api;
`docs/kernel-services/vfs/design.md`, "Switching the root";
`docs/userland/design.md` (the console shell).

### lxpty can fail, and fails at the check it names

A new feature is proven by its acceptance test, not a probe, but the
job-control checks were mutation-tested once to see that they are not
vacuous: with `tty_write_allowed` returning 0 (TOSTOP ignored) the aarch64
debug boot fails with `lxpty: FAIL the session child ended early, status
13568` -- exit 53, the TOSTOP-stop check. The first version of that run
hung instead of failing (the parent held its own copy of the child's
control pipe, so it never saw the child exit); the parent now closes it.

### Console job control and `tty` (local, release, test-net-login)

| | x86-64 | AArch64 |
|---|---|---|
| ash on the console: `sleep 100 &`, `jobs` Running, `kill %1`; `sleep 30`, ^Z Stopped, `bg` Running | PASS | PASS |
| `readlink /proc/self/fd/0` | `/dev/console` | `/dev/console` |

### PR 2 validation (local, macOS host, QEMU TCG; one sequential run)

| Item | x86-64 | AArch64 |
|---|---|---|
| host-test | pass | pass |
| analyze | clean | clean |
| debug `test`, `QEMU_SMP=1` | PASS 133.6s | PASS 143.3s |
| debug `test`, `QEMU_SMP=2` | PASS 151.0s | PASS 141.3s |
| debug `test`, `QEMU_SMP=4` | PASS 144.0s | PASS 148.0s |
| `test-smp2` | PASS 180.0s | PASS 153.5s |
| `test-chaos` | PASS 153.9s | PASS 149.6s |
| `test-harness-retry` | PASS 143.9s | PASS 166.5s |
| release build and `test` | PASS 17.7s | PASS 20.8s |
| `test-net-login` (release; console job control) | PASS 16.3s | PASS 23.2s |
| `fuzz` | pass (host binaries, one run) | -- |

## PR 3 (as built)

| Area | Change | Files |
|---|---|---|
| dropbear | 2026.94 (`dropbear-2026.94.tar.bz2`, SHA-256 `e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81`, upstream's published sum; second URL the `pinned-sources` release copy) built against the BusyBox port's musl sysroot: static, stripped, no zlib, no syslog, no login records. `localoptions.h`: public-key authentication only (password authentication compiled out), Ed25519 host keys, no forwarding, no X11, no sftp, no MOTD. `fetch-pinned.py` sends its own User-Agent (upstream answers Python's default with 403). | `ports/dropbear/`, `scripts/fetch-pinned.py` |
| The service | `/etc/dropbear/start`: make the host key with `dropbearkey -t ed25519` at the first boot of the installed system (`getrandom` blocks until the pool is seeded, M1), keep it, `exec dropbear -F -p 22 -r <key>`. `cosmo-install` writes `/etc/passwd` (`root:*:0:0:root:/root:/bin/sh`), `/etc/group`, `/root/.ssh/authorized_keys` (0600 in 0700) and `/etc/svc/dropbear` (`restart always`). The live image starts no ssh server. | `ports/dropbear/start`, `userland/system/cosmo-install.c` |
| Tests | `test-net-login` d: the harness's key in `authorized_keys`, ssh port forwarded by QEMU, `ssh root@... 'uname -a; echo ok'`, then `ssh -tt` in a host pty sized 37x101: `tty`, `stty size`, `sleep 100 &`, `jobs`, `kill %1`, `wait`; e: reboot the installed disk and log in again with `StrictHostKeyChecking=yes` against the first boot's `known_hosts`. CI: `openssh-client`, the ports cache keyed on `dropbear.mk`. | `tests/boot/net_login_test.py`, `Makefile`, `.github/workflows/ci.yml` |

Built binaries (release, both stripped and statically linked):

| | x86-64 | AArch64 |
|---|---|---|
| `dropbear` | 407,024 bytes | 359,496 bytes |
| `dropbearkey` | 195,856 bytes | 184,752 bytes |

### Acceptance a-e (local, macOS host, QEMU TCG, release `test-net-login`)

| Step | x86-64 | AArch64 |
|---|---|---|
| a. DHCP | `eth0 10.0.2.15/24 gateway 10.0.2.2 dns 10.0.2.3`, lease 86400 s; `resolv.conf` names 10.0.2.3 | the same |
| b. by address | 1 MiB in 0.2 s, SHA-256 matches | 1 MiB in 0.2 s, SHA-256 matches |
| c. by name | `nslookup example.com` -> 104.20.23.154; `Example Domain`, attempt 1 | the same, attempt 1 |
| d. ssh | key login as root, `Linux cosmo 6.0.0-cosmo CosmoOS 0.0.1 ... x86_64`, `ok`; `ssh -tt`: `/dev/pts/0`, `stty size` 37 101, `sleep 100 &` / `jobs` / `kill %1` / `wait` | the same, `aarch64` |
| e. reboot | host key kept (`StrictHostKeyChecking=yes` passed), 5.2 s | the same, 8.3 s |
| whole test | PASS in 49.3 s | PASS in 58.1 s |

### PR 3 validation (local, macOS host, QEMU TCG; one sequential run, on the final stack: main with #365 and #352 plus this PR)

| Item | x86-64 | AArch64 |
|---|---|---|
| host-test | pass | pass |
| analyze | clean | clean |
| debug `test`, `QEMU_SMP=1` | PASS 133.7 s | PASS 133.5 s |
| debug `test`, `QEMU_SMP=2` | PASS 143.7 s | PASS 154.5 s |
| debug `test`, `QEMU_SMP=4` | PASS 161.7 s | PASS 156.6 s |
| `test-smp2` | PASS 154.3 s | PASS 144.7 s |
| `test-chaos` | PASS 165.1 s | PASS 151.0 s |
| `test-harness-retry` | PASS 170.4 s | PASS 153.0 s |
| release build and `test` | PASS 17.8 s | PASS 20.7 s |
| `test-net-login` (release; acceptance a-e) | PASS 53.1 s | PASS 58.9 s |
| `fuzz` | pass | -- |

