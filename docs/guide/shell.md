# First boot and the shell

## Booting the installed system

From now on you boot `disk.img` alone. The `hostfwd` option forwards
port 2222 on your computer to the system's ssh server, which [ssh](ssh.md)
uses; it does no harm before then. On x86-64:

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

The boot log ends with init switching to the disk:

```text
init: switched to the disk root ... (root=boot-disk)
CosmoOS userland ready
```

and the console shell's prompt. On the first boot `/etc/rc` also makes
the ssh server's host key, which takes a moment.

## The shell

The console shell of an installed system is BusyBox `ash` (BusyBox 1.37,
built statically against musl): a POSIX shell with history, line
editing, `$(...)`, functions and job control. It runs as root, in `/`.

<!-- test: console -->
```console
cosmo$ echo $0
...sh
cosmo$ id
uid=0(root) gid=0(root)...
cosmo$ uname -s -m
Linux ...
cosmo$ ls /
bin   boot  dev   etc   mnt   proc  root  run   sbin  tmp   usr   var
```

`uname` says `Linux`: BusyBox is a static Linux program, and CosmoOS
runs it under its Linux personality, which answers as Linux does. The
kernel also logs each program it starts and the status it exits with
(`[ INFO] process: pid N 'id' exited with status 0`) on the console,
between the lines the programs print.

The commands are BusyBox applets, linked in `/bin`, and CosmoOS's own
tools in `/sbin`. The full list:

<!-- test: console -->
```console
cosmo$ ls /bin
...wget...
```

The applets are `sh` (ash) and `[ awk basename cat chmod clear cmp cp
cut date dd df diff dirname du echo env expr false find free grep gunzip
gzip head hexdump id less ln ls md5sum mkdir mv nslookup ping printf pwd
readlink rm rmdir sed seq sha256sum sleep sort stat stty tail tar tee
test top touch tr true tty uname uniq uptime vi wc wget which whoami
xargs` (`ports/busybox/applets`); `/sbin` holds CosmoOS's own
`cosmo-install`, `dmesg`, `fsctl`, `hostname`, `kill`, `mount`, `netcfg`,
`pkg`, `priv`, `ps`, `svc`, `sysctl`, `umount` and `vmctl`.

Files you write are on the disk and stay there:

<!-- test: console -->
```console
cosmo$ echo kept > /root/note.txt && sync
cosmo$ cat /root/note.txt
kept
```

## Job control

`ash` has job control on the console and in an ssh session: `&`,
`jobs`, `fg`, `bg`, `kill %N`, `wait`, `Ctrl-Z` to stop the foreground
job and `Ctrl-C` to interrupt it.

<!-- test: console -->
```console
cosmo$ sleep 60 &
...
cosmo$ jobs
[1]+  Running                    sleep 60
cosmo$ kill %1
cosmo$ wait
...
cosmo$ jobs
(no output)
```

## What is missing compared with Linux

The shell and its applets behave as on Linux, but the system around them
is smaller. Among what you will notice: no `ping6`; `top` shows no
process memory sizes; no hard links (`ln` without `-s`); no `/sys`; `wget` speaks HTTP only; one
user, root; no swap. [Known limitations](limitations.md) has the full
list.

Power off with `exit`; QEMU exits once the disk is synced:

<!-- test: console -->
```console
cosmo$ exit
...init: disk root synced
```

Next: [Networking](networking.md).
