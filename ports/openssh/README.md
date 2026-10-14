# ports/openssh

OpenSSH's `sftp-server`, and nothing else from OpenSSH (the scp-sftp
unit, 2026-10-14). The ssh server is dropbear (`ports/dropbear/`), which
runs an external program for the `sftp` subsystem and ships none of its
own; current OpenSSH `scp` clients use that subsystem too. It is OpenSSH's
because that is the implementation every `sftp` and `scp` client is
written against.

| Source | Version | SHA-256 |
|---|---|---|
| OpenSSH portable | 10.6p1 (`openssh-10.6p1.tar.gz`, released 2026-10-06) | `a9dc9565dffe8640f64d863cd29a32bc4a3dbdec0566a7fc44c5d6ee767d5f39` (the release notes' `SHA256 (openssh-10.6p1.tar.gz) = qdyVZd/+hkD2TYY80poyvEo9vewFZqf8RMXW7nZ9Xzk=`, base64 there) |

`scripts/fetch-pinned.py` fetches it once into `$(PORTS_CACHE)` from
`cdn.openbsd.org`, then the copy in this repository's `pinned-sources`
release, checking the digest whichever served it (`ports/busybox/README.md`).

## The build (`openssh.mk`)

Against the BusyBox port's musl sysroot and compiler-rt builtins, through
its `cc` wrapper (a static link), as dropbear is: `configure
--host=<arch>-linux-musl --without-openssl --without-zlib --without-pam
--without-pie --without-hardening --without-stackprotect`, no login
records, then `make sftp-server` alone, stripped with `llvm-objcopy`.
`sftp-server` needs neither OpenSSL nor zlib; PIE and the hardening
flags are off for the reason dropbear's are (the wrapper's fixed image
base rules out a static-pie). `-ffile-prefix-map` keeps the build
directory out of the binary (`make reproducible`, which builds it in both
trees through `OSSH_DIR`).

## On the system

`/usr/libexec/sftp-server` (mode 0755: the boot archive's `usr/libexec/`
entries keep their path, `kernel-services/vfs/ramfs.c`), copied to the
installed root with the rest of `/usr`. dropbear runs it as the `sftp`
subsystem (`SFTPSERVER_PATH`), through root's shell, in the session's
privileges (`net cred`: see `docs/guide/privileges.md`).

It makes itself undumpable at start (`prctl(PR_SET_DUMPABLE, 0)`) and
refuses to run when it cannot; the Linux door has had that one `prctl`
option since this port (`compat/linux/syscalls.c`, `lx_prctl`).
