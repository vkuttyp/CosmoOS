# Packages

`pkg` installs signed packages from repositories. An installed system
has two, named in `/etc/pkg/repos.conf`: `local`, `/var/lib/pkg/repo`,
where the installer copied the live image's repository (the packages the
image was built with), and `cosmoos`, the project's network repository,
`https://vkuttyp.github.io/CosmoOS/repo/ARCH` (`x86_64` or `aarch64`),
which each release republishes ([Network repositories](#network-repositories)
below). `pkg` checks every index and every package against the public
keys in `/etc/pkg/keys`, and refuses anything signed by another key or
whose checksum does not match.

Boot the installed system:

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

<!-- test: console -->
```console
cosmo$ cat /etc/pkg/repos.conf
local file:///var/lib/pkg/repo
cosmoos https://vkuttyp.github.io/CosmoOS/repo/...
cosmo$ pkg update
pkg: index updated: ... packages from ...
cosmo$ pkg search
...hello...prints a greeting
cosmo$ pkg install hello
pkg: installing hello-1.1 ...
cosmo$ hello
hello, world (hello 1.1)
cosmo$ pkg list
hello                1.1        prints a greeting
cosmo$ pkg info hello
...name:...hello
cosmo$ pkg remove hello
pkg: removing hello...
cosmo$ pkg install fortune
...pkg: installing fortunes-1.0...
...pkg: installing fortune-1.0...
cosmo$ pkg verify
pkg: verify: 0 problems
```

`pkg install` installs dependencies first (`fortune` needs `fortunes`),
`pkg remove` refuses to remove a package another one needs, and `pkg
verify` checks every installed file against the package database in
`/var/db/pkg`. The other commands are `pkg upgrade` and `pkg install
NAME=VERSION`. Installing, upgrading and removing need the `sys_admin`
privilege, which the console's shell has; `pkg update`, `search`, `info`,
`list` and `verify` do not. Power off:

<!-- test: console -->
```console
cosmo$ exit
...init: disk root synced
```

## Network repositories

`pkg update` reads both: the disk's repository needs nothing, the network
one needs the machine's network (QEMU's user networking is enough) and a
clock that is right. Without the network it says `pkg: cosmoos: not
reached; its last index is kept` and `from 1 of 2 repositories`, and goes
on with the disk's packages (exit 0). A server it refuses (its certificate,
a signature, an old or expired index) is exit 3.

A line of `/etc/pkg/repos.conf` is a name, a URL and options:

```text
# NAME  URL                                   [pin sha256:HEX] [insecure]
local   file:///var/lib/pkg/repo
mine    https://packages.example.org/cosmoos/x86_64
```

`pkg update` fetches each repository's `INDEX` over HTTPS through
`cosmo-fetch`, which checks the server's certificate as `wget` does,
against `/etc/ssl/certs/ca-certificates.crt`, before it sends anything.
`pin sha256:HEX` also requires the server's key to have that SHA-256
(what `openssl x509 -pubkey -noout -in cert.pem | openssl pkey -pubin
-outform der | sha256sum` prints for its certificate). Plain `http://` is
refused unless the line says `insecure`. `pkg install` fetches the
packages it needs into `/var/cache/pkg` and checks each against the index
before installing it.

A network repository's index carries a serial number and an expiry date,
and `pkg update` refuses one older than the last it accepted from that
repository, or one that has expired:

```text
pkg: mine: INDEX: serial 10 is older than 11, the last accepted from this repository (a replayed index)
pkg: mine: the index (serial 12) expired at 2027-01-16T12:00:00Z, so the repository must be signed again by its publisher before pkg update accepts it; the stored index is unchanged
pkg: mine: INDEX: the clock (2000-01-01T00:05:12Z) is before the index was published (2026-10-18T12:00:00Z): set the clock (date -s) and update again
```

An expired index is the publisher's to fix: an index is signed for 90
days and the project's is signed again every week and at every release,
so an expiry means that has stopped (for the project's repository, open
an issue). The last means the machine's clock is wrong: set it with `date -s
'2026-10-18 12:00'` and run `pkg update` again. A refused
server or index leaves the index `pkg` already had.

Next: [Upgrading](upgrading.md).
