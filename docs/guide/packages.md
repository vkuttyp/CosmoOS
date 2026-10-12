# Packages

`pkg` installs signed packages from a repository. On an installed system
the repository is `/var/lib/pkg/repo`: the installer copied the live
image's repository there, and `/etc/pkg/repos.conf` names it. `pkg`
checks the repository's index and every package against the public keys
in `/etc/pkg/keys`, and refuses anything signed by another key or whose
checksum does not match. The repository holds the packages the image was
built with; there is no network repository in 1.0.

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
NAME=VERSION`. Power off:

<!-- test: console -->
```console
cosmo$ exit
...init: disk root synced
```

Next: [Privileges](privileges.md).
