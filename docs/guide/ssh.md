# ssh

An installed system runs the dropbear ssh server (2026.94, static, on
port 22) as a service. It accepts **root** only, by **public key** only:
root has no password (its `/etc/passwd` entry is `root:*:...`), and
password authentication is compiled out of dropbear. You log in with an
Ed25519 or an RSA key, so an existing `~/.ssh/id_rsa.pub` works (RSA with
SHA-256 signatures, `rsa-sha2-256`, which current OpenSSH clients offer;
not the old SHA-1 `ssh-rsa`). The server's own host key is Ed25519:
`/etc/dropbear/dropbear_ed25519_host_key`, made at the system's first
boot and kept across reboots. `scp` and `sftp` copy files both ways
(below); there is no port forwarding. The live image runs no ssh server.

## A key

Make a key pair on your computer and serve its public half, as on the
previous page:

<!-- test: host -->
```sh
ssh-keygen -q -t ed25519 -N "" -f cosmo_key
python3 -m http.server 8000 --bind 127.0.0.1 >/dev/null 2>&1 &
sleep 1
```

Boot the installed system (the `hostfwd` option in these commands is
what makes port 2222 on your computer reach port 22):

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

and put the public key in root's `authorized_keys`:

<!-- test: console -->
```console
cosmo$ wget -q -O /root/.ssh/authorized_keys http://10.0.2.2:8000/cosmo_key.pub
cosmo$ chmod 600 /root/.ssh/authorized_keys && cat /root/.ssh/authorized_keys
ssh-ed25519 ...
```

<!-- test: host -->
```sh
kill $!
```

## Logging in

Wait for the server: right after a boot it may still be starting, and
until it listens QEMU accepts the connection and closes it. This tries
for up to 30 seconds:

<!-- test: host -->
```sh
for i in 1 2 3 4 5 6 7 8 9 10; do ssh -i cosmo_key -p 2222 -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=known_hosts -o ConnectTimeout=5 root@127.0.0.1 true && break; sleep 3; done
```

Run a command:

<!-- test: host -->
```sh
ssh -i cosmo_key -p 2222 -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=known_hosts root@127.0.0.1 cat /root/note.txt
```

The first connection records the host key in `known_hosts`; later ones
check it. Or log in to a shell (`-tt` asks for a terminal even when, as
here, the command's input is not one):

<!-- test: session prompt="~ # " -->
```sh
ssh -tt -i cosmo_key -p 2222 -o UserKnownHostsFile=known_hosts root@127.0.0.1
```

<!-- test: console -->
```console
~ # tty
/dev/pts/...
~ # echo $HOME
/root
~ # exit
```

The prompt shows the current directory (`~`, root's home) and `#` for
root. The session is BusyBox `ash` on a pseudo-terminal, with job control as on
the console. It holds fewer privileges than the console does: see
[Privileges](privileges.md).

## Copying files

`scp` copies files to and from the system, and `sftp` does the same
interactively (or from a batch file). Note the capital `-P` for the
port, where `ssh` takes `-p`:

<!-- test: host -->
```sh
echo "from my computer" > to-cosmo.txt
scp -i cosmo_key -P 2222 -o UserKnownHostsFile=known_hosts to-cosmo.txt root@127.0.0.1:/root/
scp -i cosmo_key -P 2222 -o UserKnownHostsFile=known_hosts root@127.0.0.1:/root/to-cosmo.txt back.txt
cmp to-cosmo.txt back.txt && echo same
```

<!-- test: host -->
```sh
printf 'ls -l /root\nget /root/note.txt note-copy.txt\nrm /root/to-cosmo.txt\n' > batch.txt
sftp -i cosmo_key -P 2222 -o UserKnownHostsFile=known_hosts -b batch.txt root@127.0.0.1
cat note-copy.txt
```

Current OpenSSH `scp` speaks the SFTP protocol to the server; `scp -O`
asks for the older scp protocol, which works too. The server side is
OpenSSH's `sftp-server` for the first and dropbear's `scp` for the
second.

Power off from the console:

<!-- test: console -->
```console
cosmo$ exit
...init: disk root synced
```

Next: [Packages](packages.md).
