# Networking

CosmoOS configures its network itself. The kernel's DHCP client asks for
an address on each interface as the interface comes up (`eth0` is the
first virtio-net or e1000e NIC); `/etc/rc` then runs `netcfg`, which
waits for the lease and writes the DNS servers it names to
`/etc/resolv.conf`. On QEMU's user-mode network the address is always
`10.0.2.15/24`, the gateway and your computer are `10.0.2.2`, and the DNS
server is `10.0.2.3`.

Boot the installed system as on the previous page. On x86-64, then on
AArch64:

<!-- test: session x86_64 prompt="cosmo$ " -->
```sh
qemu-system-x86_64 -machine q35 -cpu qemu64,+nx -smp 2 -m 512M \
    -drive if=pflash,format=raw,readonly=on,file=efi-x86_64.fd \
    -drive format=raw,file=disk.img \
    -device virtio-rng-pci \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 -device virtio-net-pci,netdev=net0 \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -nographic -no-reboot
```

On AArch64:

<!-- test: session aarch64 prompt="cosmo$ " -->
```sh
qemu-system-aarch64 -machine virt -cpu cortex-a72 -smp 2 -m 512M \
    -drive if=pflash,format=raw,readonly=on,file=efi-aarch64.fd \
    -drive if=none,id=boot,format=raw,file=disk.img -device virtio-blk-pci,drive=boot \
    -device virtio-rng-pci \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 -device virtio-net-pci,netdev=net0 \
    -semihosting-config enable=on,target=native \
    -nographic -no-reboot
```

## Looking at the configuration

<!-- test: console -->
```console
cosmo$ netcfg show
eth0: bound 10.0.2.15/24 gateway 10.0.2.2 server 10.0.2.2 lease ... dns 10.0.2.3
cosmo$ cat /etc/resolv.conf
# written by netcfg from the DHCP lease
nameserver 10.0.2.3
```

`netcfg show` prints each interface's lease: address, gateway, the DHCP
server, how long the lease runs and when it renews. `netcfg` alone (what
`/etc/rc` runs) waits up to ten seconds for a lease (`--wait S` changes
that) and rewrites `/etc/resolv.conf`, once per boot: a lease renewed
later with other DNS servers does not update the file.

`ip` shows the interfaces, routes and neighbours as Linux's iproute2 does,
and BusyBox's `ifconfig`, `route`, `netstat` and `arp` read the same
state:

<!-- test: console -->
```console
cosmo$ ip a
1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65535 state UNKNOWN
    inet 127.0.0.1/8 scope host lo
2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 state UP
    link/ether ... brd ff:ff:ff:ff:ff:ff
    inet 10.0.2.15/24 brd 10.0.2.255 scope global dynamic eth0
cosmo$ ip r
default via 10.0.2.2 dev eth0 proto dhcp src 10.0.2.15
10.0.2.0/24 dev eth0 proto kernel scope link src 10.0.2.15
cosmo$ route -n
Kernel IP routing table
0.0.0.0         10.0.2.2        0.0.0.0         UG    0      0        0 eth0
cosmo$ netstat -tln
tcp        0     16 0.0.0.0:22              0.0.0.0:*               LISTEN...
cosmo$ hostname
cosmo
```

`ip` only shows: it is CosmoOS's own, because BusyBox's `ip` needs
netlink, which the kernel does not have. `ifconfig eth0 down` and
`ifconfig eth0 up` take the link down and up again, and `ifconfig eth0
10.0.2.20 netmask 255.255.255.0` sets an address by hand until the DHCP
lease changes; each needs the `net` privilege (see
[Privileges](privileges.md)). The machine is called `cosmo` until a DHCP
server names it: `netcfg` then writes that name to `/etc/hostname`, which
the next boot keeps. `hostname NAME` renames it until the next boot; put
the name in `/etc/hostname` to keep it.

## Fetching files

BusyBox `wget` fetches over HTTP and HTTPS. Serve this directory from your computer
on port 8000 (QEMU's user network reaches your computer's `127.0.0.1` as
`10.0.2.2`):

<!-- test: host -->
```sh
python3 -m http.server 8000 --bind 127.0.0.1 >/dev/null 2>&1 &
sleep 1
```

and fetch the checksum file from CosmoOS:

<!-- test: console -->
```console
cosmo$ wget -q -O /tmp/sums http://10.0.2.2:8000/SHA256SUMS && cat /tmp/sums
...  cosmoos-....img
```

Names are resolved through `/etc/resolv.conf`; with QEMU's user network
that reaches the internet through your computer:

<!-- test: console -->
```console
cosmo$ nslookup example.com
...Name:...example.com
cosmo$ wget -q -O - http://example.com/
...Example Domain...
```

HTTPS works the same way, and the server's certificate is checked:

<!-- test: console -->
```console
cosmo$ wget -q -O - https://example.com/
...Example Domain...
```

`wget` hands the connection to `/bin/ssl_client` (mbedTLS), which
accepts a certificate only if it chains to one of the certificate
authorities in `/etc/ssl/certs/ca-certificates.crt` (Mozilla's list, as
curl publishes it, fixed when the image was built), names the host in
the URL, and is valid at the system's time. Otherwise the fetch fails
and `ssl_client` says why:

```text
cosmo$ wget -q -O /tmp/x https://10.0.2.2:8443/
ssl_client: 10.0.2.2: the server's certificate is not accepted:
  The certificate is not correctly signed by the trusted CA
wget: error getting response
```

- Another set of certificate authorities, such as your own: name its
  PEM file in `SSL_CERT_FILE` (`env SSL_CERT_FILE=/root/my-ca.pem wget ...`).
- `wget --no-check-certificate` skips the check, and `ssl_client` prints
  a line saying so. The connection is then encrypted to whoever answers.
- The validity check uses the wall clock, which comes from the RTC or
  firmware at boot (there is no NTP): with a clock far off, every
  certificate looks expired or not yet valid.
- BusyBox's own TLS code checks no certificate; this `wget` does not use
  it.

`ping` sends ICMP echo requests (IPv4 only; no `ping6`). QEMU's user
network answers a ping to its gateway itself; whether a ping to the
internet gets through depends on your computer, since QEMU forwards
those as its own ICMP, which some hosts do not allow:

<!-- test: console -->
```console
cosmo$ ping -c 3 10.0.2.2
PING 10.0.2.2 (10.0.2.2): 56 data bytes
64 bytes from 10.0.2.2: seq=0 ttl=...
...3 packets transmitted, 3 packets received, 0% packet loss...
```

Stop the file server and power off:

<!-- test: host -->
```sh
kill $!
```

<!-- test: console -->
```console
cosmo$ exit
...init: disk root synced
```

Next: [ssh](ssh.md).
