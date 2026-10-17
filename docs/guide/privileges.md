# Privileges

Being root (uid 0) is not enough on CosmoOS: each privileged operation
needs its own privilege, and a process holds a set of them. There are
six:

| Privilege | Allows |
|---|---|
| `mount` | `mount`, `umount`, `switch_root`, mount namespaces, giving a child its own root, `chroot` |
| `sys_admin` | `sysctl` writes, the block and filesystem control devices (`/dev/blkctl`, `/dev/fsctl`), the kernel log, domains and uts namespaces, overriding file permissions and ownership (`chown` to another owner) |
| `net` | network configuration (`/dev/net/netctl`) and binding ports below 1024 |
| `proc` | signalling, seeing and inspecting any process; raising resource limits |
| `cred` | starting a process with other ids; changing uid, gid and groups |
| `device` | opening raw device nodes (`/dev/net/tap`, `/dev/vmm`) |

(`kernel/include/uapi/cosmo/syscall.h`, `COSMO_PRIV_*`.)
init starts with all six. A child inherits its parent's set; a set can
only shrink, and nothing adds one back. It shrinks by dropping
privileges at spawn (`priv` below, a service's `privileges` line), or by
giving up uid 0 entirely: when none of the real, effective and saved uids
is 0 any more, the set is emptied. A privilege is also used only while
the effective uid is 0, so a process that changes just its effective uid
keeps its set, unusable until it switches back. Boot the installed
system:

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

`priv` shows the set, and runs a command with some dropped (`-d`) or
with only some kept (`-k`):

<!-- test: console -->
```console
cosmo$ priv
mount sys_admin net proc cred device
cosmo$ priv -d mount -- priv
sys_admin net proc cred device
cosmo$ priv -k net -- mount -t ramfs none /mnt
mount: permission denied (are you root?)
```

Services get theirs from the `privileges` line of their file in
`/etc/svc`. The ssh server's holds only `net` (to listen on port 22) and
`cred` (to set up a session), so an ssh login, though it is root, has
just those two (the loop first waits for the server to listen, as on the
[ssh](ssh.md) page):

<!-- test: console -->
```console
cosmo$ cat /etc/svc/dropbear
...
privileges net cred
...
```

<!-- test: host -->
```sh
for i in 1 2 3 4 5 6 7 8 9 10; do ssh -i cosmo_key -p 2222 -o UserKnownHostsFile=known_hosts -o ConnectTimeout=5 root@127.0.0.1 true && break; sleep 3; done
ssh -i cosmo_key -p 2222 -o UserKnownHostsFile=known_hosts root@127.0.0.1 priv | grep -x 'net cred'
```

prints `net cred`.

The console is where the machine is administered. Note that a root
session with fewer privileges can still edit files in `/etc` that a
later boot runs with all of them ([Known limitations](limitations.md)).
Power off:

<!-- test: console -->
```console
cosmo$ exit
...init: disk root synced
```

Next: [Known limitations](limitations.md).
