# ports/dropbear

The ssh server of the installed system (roadmap M4, decision 6): static
dropbear, built from source for the architecture in hand by the same
ports path as BusyBox (`ports/busybox/`), and started by the service
manager on the installed root only.

| Source | Version | SHA-256 |
|---|---|---|
| dropbear | 2026.94 (`dropbear-2026.94.tar.bz2`) | `e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81` (the release's published `SHA256SUM.asc`, the same on matt.ucc.asn.au and dropbear.nl) |

`scripts/fetch-pinned.py` fetches it once into `$(PORTS_CACHE)`: the
upstream URL first, then the copy in this repository's
[`pinned-sources`](https://github.com/vkuttyp/CosmoOS/releases/tag/pinned-sources)
release, the digest checked whichever served it (`ports/busybox/README.md`).
The upstream answers Python's default User-Agent with 403, so fetch-pinned
sends one of its own.

## The build (`dropbear.mk`)

Against the BusyBox port's musl sysroot and compiler-rt builtins, through
its `cc` wrapper (a static link with musl's start files, the image at 4
MiB), so nothing is built twice: `configure --host=<arch>-linux-musl
--enable-static --disable-harden --enable-bundled-libtom`, no zlib, no
syslog, and no login records (`--disable-lastlog --disable-utmp
--disable-utmpx --disable-wtmp --disable-wtmpx --disable-loginfunc
--disable-pututline --disable-pututxline`). Hardening is off because its
`-pie` would ask for a static-pie, which the wrapper's fixed image base
rules out. `make PROGRAMS="dropbear dropbearkey scp" STATIC=1` (scp since
the scp-sftp unit); all three stripped with `llvm-objcopy`. `-ffile-prefix-map` keeps the build directory out of
the binaries (`make reproducible`).

`localoptions.h` is the configuration, each line with its reason:
public-key authentication only -- password authentication is compiled
out, not refused; an Ed25519 host key, the only one the service names
(`-r`); user keys Ed25519 or RSA (RSA on since v1.0.2 so an existing
`id_rsa.pub` works, with `rsa-sha2-256` signatures only -- dropbear has
no `rsa-sha2-512` -- `DROPBEAR_RSA_SHA1 0`; ECDSA and DSS compiled out);
no TCP or agent forwarding, no X11, no MOTD, no inetd mode. Files (the
scp-sftp unit): the `sftp` subsystem is on (`DROPBEAR_SFTPSERVER 1`) and
runs `SFTPSERVER_PATH`, `/usr/libexec/sftp-server` -- OpenSSH's, from
`ports/openssh`, dropbear providing none -- and scp's classic protocol
runs this source's `scp` from `/bin` through the session's shell.

## On the system

The image carries `/sbin/dropbear`, `/sbin/dropbearkey`, `/bin/scp` and
`/etc/dropbear/start` (and `ports/openssh` adds `/usr/libexec/sftp-server`). `cosmo-install` adds what the installed root needs:
`/etc/passwd` (`root:*:0:0:root:/root:/bin/sh`: one user, no password to
check), `/etc/group`, `/root/.ssh/authorized_keys` (empty, 0600, in a
0700 directory) and `/etc/svc/dropbear` (`exec /bin/sh /etc/dropbear/start`,
`restart always`). The live image starts no ssh server.

`/etc/dropbear/start` makes the host key at the first boot of the installed
system and keeps it: `dropbearkey -t ed25519` draws it from `getrandom`,
which blocks until the kernel's pool is seeded (roadmap M1), so no key is
made from an unseeded pool -- and dropbear seeds itself the same way before
it accepts a connection. Then `exec dropbear -F -p 22 -r <key>`: in the
foreground under `svc`, logging to stderr, which `svc` keeps in
`/var/log/svc/dropbear` (there is no `-E` without syslog, and no `-s`
without password logins).

A login runs root's shell, BusyBox ash, as a login shell on a pseudo-terminal
(`/dev/ptmx`, `/dev/pts/N`; docs/kernel/tty/design.md, "Pseudo-terminals")
when the client asks for one, with job control; dropbear makes the slave
the session's controlling terminal (`setsid`, `TIOCSCTTY`) and passes the
client's window size (`TIOCSWINSZ`).
