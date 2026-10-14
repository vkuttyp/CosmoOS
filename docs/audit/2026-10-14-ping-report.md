# ping: raw ICMP sockets, the socket protocol, and an interval timer — 2026-10-14

The second of the owner's post-1.0 units, moved ahead of scp and sftp on
2026-10-14 when `ping` was found missing on Parallels Desktop. Base: the
everyday commands (PR #381, `everyday-commands` 42067c7a). Goal: BusyBox
`ping` works as on Linux, to the local network and the internet.

## What `ping` needed

| Need | Before | Now |
|---|---|---|
| a socket `ping` can open | BusyBox 1.37's `ping` opens only `socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)` and parses each reply behind its IPv4 header; the Linux door refused `SOCK_RAW` (`-ESOCKTNOSUPPORT`) | raw ICMP sockets over IPv4, for a caller holding `PRIV_NET` |
| replies reaching a program | an incoming echo reply went only to the self-tests' hook and was freed (`icmp_input`) | every ICMP message the stack accepted is copied to each raw socket, IPv4 header in front (N31) |
| a second echo | BusyBox sends each echo after the first from `SIGALRM`; `setitimer`, `getitimer` and `alarm` were `-ENOSYS`, so `ping -c 3` got one reply and then waited | `ITIMER_REAL` per process |
| the applet | not built | `CONFIG_PING`, `CONFIG_FEATURE_FANCY_PING`; no `ping6` |

Linux's unprivileged "ping sockets" (`SOCK_DGRAM`, `IPPROTO_ICMP`) were
the first plan; the survey of BusyBox's source showed `ping` never opens
one, so raw sockets, which need no patch to BusyBox, were built instead.

## Changes

| Area | Change | Files |
|---|---|---|
| raw sockets | `struct raw_pcb` on a list under a spinlock; `raw_icmp_input` takes a wake reference on each socket under the lock and copies after it; a queue of 64; sends go out unchanged through `ipv4_output` (the host firewall records the echo and admits its reply); `bind`/`connect`/`listen` `-EOPNOTSUPP` | `kernel-services/network/raw.c`, `kernel/include/kernel/net/raw.h` |
| socket layer | `COSMO_SOCK_RAW`, `ksock_create_raw` (`PRIV_NET`), a raw branch in every path that took "not a datagram" for TCP; both doors send a raw socket's message whole | `socket.c`, `socket.h`, `native.c`, `compat/linux/syscalls.c` |
| ICMP input | `raw_icmp_input` after the checksum and the firewall's quiet check, before the echo reply rewrites the mbuf | `ipv4.c` |
| the Linux door's `socket` | reads its protocol: 0 or the type's own, `IPPROTO_ICMP` for raw, else `-EPROTONOSUPPORT` (a defect: a ping socket used to become a UDP one) | `compat/linux/syscalls.c` |
| interval timer | one kernel timer per process, `SIGALRM` from its callback (interrupt context, as ^C's), re-armed by the interval; setters cancel synchronously under `itimer_lock`; fork disarmed, exec kept, exit cancels | `kernel/process/itimer.c`, `process.h`, `process.c` |
| Linux syscalls | `setitimer`, `getitimer` (x86-64 38/36, AArch64 103/102), `alarm` (x86-64 37) | `compat/linux/` |
| errnos | `EDESTADDRREQ` 89, `EPROTONOSUPPORT` 93 (kernel, native ABI, libc) | |

## Tests

- `net-raw-icmp` (kernel self-test, N31): refusals; an echo to
  `127.0.0.1` read back raw, request and reply, with a `0x45` IPv4 header,
  from `127.0.0.1` port 0; the stack still answers; a full queue drops the
  copy and not the stack's message.
- `lxtest`: the protocol checks (a ping socket is `-EPROTONOSUPPORT`, UDP
  and TCP by number open, raw TCP and raw ICMPv6 refused, raw ICMP opens);
  `setitimer`/`getitimer` (get, set, clear, the old value, `-EINVAL`, a
  20 ms timer making `SIGALRM` pending and disarming) and `alarm`'s
  rounding on x86-64.
- `tests/busybox/ash.sh`: `ping -c 3 127.0.0.1` gets three replies;
  `priv -d net ping` is refused ("permission denied").
- The networking guide (a tested page, CI on both architectures and from
  macOS): `ping -c 3 10.0.2.2`, which QEMU's user network answers itself.
- `tools/sock-proto-probe.py`: fixed `LINUXTEST: PASS`; `--old` reverses
  the protocol check and lxtest fails on it, the ping socket being
  descriptor 3 (a UDP socket), on x86-64 and AArch64 (run at 1fbf134e).

## On Parallels Desktop 27

The AArch64 release image, live and installed on NVMe: `ping -c 3` to the
gateway 10.211.55.1 (0.4-1.2 ms), to 1.1.1.1 (91-168 ms) and to
`example.com` by name, all three of three; an open-ended `ping` stopped
with ^C prints its statistics; the Mac pings the VM; over ssh,
`priv -d net ping` is refused. The first build here got one reply and no
more: that is how the missing interval timer was found.

## Validation

Local: debug `make test` and `make test-busybox` pass on x86-64 and
AArch64 (scripted test 93 checks), release images and `make fuzz` build
and pass. The full matrix is run before the PR.

## Not done here

- `ping6` and raw IPv6 (it also needs `recvmsg` ancillary data and
  `ICMP6_FILTER`); Linux's ping sockets.
- On a raw socket: `bind` (so `ping -I`), `connect`, `IP_HDRINCL`,
  `ICMP_FILTER` and other options (`setsockopt` is `-ENOPROTOOPT` for all).
- `ITIMER_VIRTUAL` and `ITIMER_PROF`.
- BusyBox's documented config regeneration (`make defconfig`) failed on
  the macOS build host; the two `ping` lines were set by hand.
