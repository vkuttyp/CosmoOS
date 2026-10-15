# Network inspection: /proc/net, the interface ioctls, `ip` — 2026-10-15

The 1.1 roadmap's S1, the first unit of the shell-completeness track the
project owner chose on 2026-10-15. Base: main `c68d6cd2` (P1 merged), with
the track's roadmap section from PR #391 under it. Goal: on the installed
system, the commands that show the network -- `ip a`, `ifconfig`, `route`,
`netstat`, `arp`, `hostname` -- work as they do on Linux, and `ifconfig`
can take a link down and up under `PRIV_NET`.

## Before

| What | Before |
|---|---|
| `ip`, `ifconfig`, `route`, `netstat`, `arp` | not built; BusyBox's `ip` speaks only netlink, which the kernel lacks |
| `/proc/net` | absent: procfs held processes and five system files |
| interface ioctls | `lx_ioctl` answered a terminal and `-ENOTTY` for every other handle |
| `hostname` | the native `/sbin/hostname` (print or set); no `sethostname` in the Linux personality; the name always `cosmo` at boot |
| DHCP option 12 | not requested, not kept |
| netctl | version 7: forwards, filter, flows, leases; nothing about the machine's own interfaces, routes or neighbours |

## Design

One rule, the network invariants' new **N32**: every view is rendered from
one of five listings, each a copy taken under its table's own lock and
formatted after the lock is released (`docs/kernel-services/network/design.md`,
"Inspecting the stack"). The listings are `netif_list` (and one interface
by name or index), `ipv4_route_list` (the routes `ipv4_route` takes: there
is no routing table), `arp_list`, `tcp_list` and `udp_list`. `tcp_list`
holds only the table lock -- a pcb's lock comes first in the order -- so it
reads queue lengths relaxed and a pcb's remote end only once its state is
past `CLOSED`.

## Changes

| Area | Change | Files |
|---|---|---|
| listings | `netif_list`, `netif_info_by_name`/`_by_index`; `ipv4_route_list`; `arp_list`; `tcp_list`; `udp_list` | `kernel-services/network/{netif,ipv4,arp,tcp,udp}.c`, their headers |
| `/proc/net` | `dev`, `route`, `arp`, `tcp`, `tcp6`, `udp`, `udp6`, `if_inet6` in Linux 6.x's layouts (`procnet.c`); procfs serves the directory, a file's text up to 64 KiB a page at a time | `kernel-services/network/procnet.c`, `kernel/include/kernel/net/procnet.h`, `kernel-services/filesystem/procfs/procfs.c` |
| interface ioctls | `SIOCGIFCONF` and the `SIOCGIF*` getters; under `PRIV_NET` `SIOCSIFFLAGS` (`UP`), `SIOCSIFADDR`, `SIOCSIFNETMASK`, `SIOCSIFBRDADDR` (the address's own only), `SIOCSIFMTU` and `SIOCSIFHWADDR` (the current value only, else `-EOPNOTSUPP`) | `compat/linux/netioctl.c`, `linux_abi.h`, `syscalls.c` |
| `sethostname` | in the Linux personality (170, AArch64 161), `PRIV_SYS_ADMIN` as the native call | `compat/linux/syscalls.c`, `nr_*.h` |
| DHCP | requests and keeps option 12 when it is 1-63 letters, digits, `-` and `.` | `kernel-services/network/dhcp.c` |
| netctl v8 | a host name in each lease (the record 64 -> 128 bytes); interface, route and neighbour sections after the lease section | `kernel/include/uapi/cosmo/netctl.h`, `kernel-services/network/tap.c` |
| `ip` | native `/sbin/ip`: `address`, `link`, `route`, `neigh`, `show [dev] NAME`, `-4`/`-6`, iproute2's layout; any change refused | `userland/system/ip.c` |
| host name | `netcfg` writes the lease's name to `/etc/hostname` and sets it; `/etc/rc` applies `/etc/hostname` first | `userland/system/netcfg.c`, `userland/etc/rc` |
| BusyBox | `ifconfig`, `route`, `netstat`, `arp`, `hostname` (ifconfig's SLIP options off: no `<linux/if_slip.h>`) | `ports/busybox/{applets,busybox.config,mkconfig.py}` |
| harness | `QEMU_NET_HOSTNAME`: slirp's DHCP names the machine | `scripts/qemu-run.sh` |

## Tests

`test-net-login`'s net boot names the machine `cosmo-s1` through slirp's
DHCP; `netcfg show` must report the name. Stage g, at the end of that
boot:

- `ip a`, `ip -4 a`, `ip r`, `ip neigh`, `ip link show eth0`; `ip addr
  add` refused.
- BusyBox `ifconfig`, `route -n`, `netstat -tuln` (dropbear on
  `0.0.0.0:22` and `:::22`), `arp -a` (the gateway at the MAC `ip neigh`
  gave), `hostname`, `hostname -s`, `/etc/hostname`.
- `ifconfig eth0 down` refused with `PRIV_NET` dropped (`EPERM`, eth0
  still up); as root down (`state DOWN`) and up, and the gateway answers a
  ping.
- Every other setter family on `eth1`: `EPERM` without `PRIV_NET` and
  nothing changed; as root `10.0.3.15/24` in `ip a`, `ip r` and
  `ifconfig`; the broadcast, MTU and MAC it has accepted, others refused.

The reboot boot's slirp names nothing, and `hostname` must still print
`cosmo-s1` (`/etc/hostname`). `net-tapctl` requires the version-8
interface section to list `lo` first, index 1, up, at 127.0.0.1/8, and
every snapshot read to have the length its seven sections' counts give.

A new feature is proven by its acceptance test (AGENTS.md); on main every
step of stage g fails at its first command (`ip: not found`).

## Validation

- `make test-net-login` on the branch: x86-64 PASS in 68.0 s, AArch64
  PASS in 72.9 s (stage g as above, every line of it; the reboot keeps
  `cosmo-s1`).
- **The validation matrix**, on `781c122d` (the boots) and `b8cf3fc4`
  (`fuzz`, `analyze` and `host-test` again, after the fuzz shim gained
  `netif_list`), every step PASS; the boot times are the harness's own
  lines:

  | Step | x86-64 | AArch64 |
  |---|---|---|
  | `host-test`, `fuzz`, `analyze` | pass | pass |
  | debug `test`, `QEMU_SMP=1` / `2` / `4` | 142.6 / 151.2 / 154.4 s | 137.3 / 148.0 / 160.6 s |
  | `test-smp2` | 144.0 s | 172.6 s |
  | `test-chaos` | 145.5 s | 164.7 s |
  | `test-harness-retry` | 152.4 s | 155.2 s |
  | `BUILD=release test` | 18.2 s | 21.7 s |
  | `BUILD=release test-busybox` | 36.1 s | 42.3 s |

  The network harness passed in every debug boot (the boot test requires
  it). The first `fuzz` run failed to link: the shim had no
  `netif_list`, which `ipv4_route_list` reads. The first `analyze` run
  refused to judge: the boots were built with swiftly's clang (first on
  `PATH`), for which there is no reviewed baseline; `analyze` was then
  run with Apple's clang, which the baseline covers.
- **Parallels Desktop 27.0.3, by hand** (`prl_s1.py` over the VM's serial
  socket), in a VM made for this (`CosmoOS S1 network`: the branch's
  AArch64 release image on a 66 MiB plain NVMe disk, a blank 512 MiB NVMe
  disk, 2 CPUs, shared network): `cosmo-install nvme0n2` installed, the
  installed disk booted (`switched to the disk root nvme0n2p2`), and on
  it: `netcfg show` `eth0: bound 10.211.55.11/24 gateway 10.211.55.1`; `ip
  a` eth0 `00:1c:42:e1:a7:d1`, `inet 10.211.55.11/24 brd 10.211.55.255
  scope global dynamic eth0`, `valid_lft 1799sec`, the link-local address;
  `ip -4 a`, `ip link`; `ip r` `default via 10.211.55.1 dev eth0 proto
  dhcp src 10.211.55.11` and the subnet; `ifconfig`; `route -n`; `netstat
  -tuln` dropbear on `0.0.0.0:22` and `:::22`; `/proc/net/dev` and
  `route`; `priv -d net -- ifconfig eth0 down` `SIOCSIFFLAGS: Operation
  not permitted`, eth0 still up; `ifconfig eth0 down` (`state DOWN`) and
  `up`, then the gateway answered two pings and `wget http://example.com/`
  worked; `ip neigh` `10.211.55.1 dev eth0 lladdr 00:1c:42:00:00:18
  REACHABLE`, `arp -a` the same entry (named `prl-local-ns-server.shared`
  by Parallels' DNS); `poweroff` stopped the VM. Parallels' DHCP sends no
  option 12, so the name stayed `cosmo` and no `/etc/hostname` was
  written, as designed. Seen on the way: the first attempt put the image
  on a 128 MiB disk, and the kernel refused the live disk's GPT (`backup
  header is not on the last sector`), so `cosmo-install` had no ESP to
  copy; the guide does not say the disk must be the image's size
  (inventory).

## Not done here (inventory §11)

- Netlink: BusyBox `ip` stays off; `/sbin/ip` is the substitute.
- `SIOCSIFMTU` and `SIOCSIFHWADDR` change nothing: IPv4 never fragments
  and TCP's MSS is a constant, so a smaller MTU would fail full-sized
  sends; a MAC change needs the device's filter.
- `route add`/`del` and `arp -s`/`-d` are `-ENOTTY`: no routing table, no
  static neighbour.
- `ip` needs `PRIV_NET` (it reads `/dev/net/tapctl`); Linux's does not.
- IPv6 neighbours are not listed; `/proc/net` sockets carry uid 0 and
  inode 0, so `netstat -p` names no program; UDP's queue is in datagrams.
