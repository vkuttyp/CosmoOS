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

(filled in from the validation matrix run)

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
