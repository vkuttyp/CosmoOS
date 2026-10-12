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
later with other DNS servers does not update the file. There is no tool
to set an address by hand while the system runs; the address is DHCP's.

## Fetching files

BusyBox `wget` fetches over HTTP. Serve this directory from your computer
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

**HTTP only.** `wget` cannot fetch `https://` URLs. BusyBox's own TLS
code checks no certificate, so it would encrypt a connection to whoever
answers -- which looks secure and is not -- and there is no TLS library
in CosmoOS to do it properly. TLS was compiled out by decision rather
than shipped unverified. Use HTTP from hosts you trust, and check what
you fetch (`sha256sum` is there).

There is no `ping`. Stop the file server and power off:

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
