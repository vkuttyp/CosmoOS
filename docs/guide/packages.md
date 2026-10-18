# Packages

`pkg` installs signed packages from a repository. On an installed system
the repository is `/var/lib/pkg/repo`: the installer copied the live
image's repository there, and `/etc/pkg/repos.conf` names it. `pkg`
checks the repository's index and every package against the public keys
in `/etc/pkg/keys`, and refuses anything signed by another key or whose
checksum does not match. That repository holds the packages the image
was built with; a repository on a server is added with one line of
`repos.conf` ([Network repositories](#network-repositories) below).

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
/var/lib/pkg/repo
cosmo$ pkg update
pkg: index updated: ... packages from 1 repository...
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
pkg: mine: INDEX: expired at 2026-11-17T12:00:00Z
pkg: mine: INDEX: the clock (2000-01-01T00:05:12Z) is before the index was published (2026-10-18T12:00:00Z): set the clock (date -s) and update again
```

The last means the machine's clock is wrong: set it with `date -s
'2026-10-18 12:00'` and run `pkg update` again. A refused
server or index leaves the index `pkg` already had.

Next: [Upgrading](upgrading.md).
