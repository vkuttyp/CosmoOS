# scp and sftp — 2026-10-14

The third of the owner's post-1.0 units (after the everyday commands and
`ping`): copying files to and from the installed system over ssh. Base:
main `ae94d281` (`ping`, PR #382). Goal: a stock OpenSSH client's `scp`,
in both of its protocols, and `sftp` work against the installed system.

## What a client needs from the server

| Client | What it runs on the server | Before |
|---|---|---|
| `scp -O` (the classic protocol) | `scp -t <dir>` or `scp -f <file>` through the session's shell | no `scp` in the image |
| `scp` (OpenSSH 9.0 and later: SFTP underneath) | the `sftp` subsystem | `DROPBEAR_SFTPSERVER 0`; dropbear ships no sftp-server |
| `sftp` | the `sftp` subsystem | the same |

## Changes

| Area | Change | Files |
|---|---|---|
| scp | dropbear's own `scp` built beside the server (`PROGRAMS="dropbear dropbearkey scp"`), installed as `/bin/scp`; the session's PATH finds it | `ports/dropbear/dropbear.mk` |
| sftp | a new pinned port, OpenSSH portable 10.6p1, building `sftp-server` alone (no OpenSSL, zlib, PAM or PIE), installed as `/usr/libexec/sftp-server`; dropbear's sftp subsystem on and pointed there | `ports/openssh/`, `ports/dropbear/localoptions.h`, `Makefile` |
| boot namespace | archive entries under `usr/` keep their path (`usr/bin/`, `usr/sbin/`, `usr/libexec/` executable) instead of landing under `/boot`: the first build put `sftp-server` at `/boot/usr/libexec/` and the subsystem answered "not found" | `kernel-services/vfs/ramfs.c` |
| Linux door | `prctl` with `PR_SET_DUMPABLE`/`PR_GET_DUMPABLE` (x86-64 157, AArch64 167), the flag kept on the process (fork inherits it, exec resets it) and reported; every other option `-EINVAL`. `sftp-server` makes itself undumpable at start and exited 255 ("unable to make the process undumpable: Function not implemented") without it | `compat/linux/syscalls.c`, `process.h`, `process.c` |
| release and CI | the port in the CI cache keys, `check-reproducible.sh` (both trees, `scp` and `sftp-server` compared), the release notes' pinned-source table | `.github/workflows/ci.yml`, `scripts/` |

## Tests

- `tests/boot/net_login_test.py` (`make BUILD=release test-net-login`,
  CI on both architectures): 256 KiB of random data to the installed
  system and back with `scp -O` and with `scp`, SHA-256 compared on both
  sides; an `sftp -b` batch doing `mkdir`, `put`, `ls -l`, `get` (the
  digest compared), `rename`, `rm` and `rmdir`, and the directory gone.
- The user guide's ssh page copies a file each way with `scp` and runs an
  `sftp` batch (CI runs the guide on Linux and macOS).
- `lxtest`: `PR_GET_DUMPABLE` is 1, `PR_SET_DUMPABLE` 0 then reads 0, 2
  is `-EINVAL`, an unknown option is `-EINVAL`.

## Validation

`make BUILD=release test-net-login` passes on x86-64 and AArch64. The rest
is filled in before the PR.

## Not done here

- Port forwarding stays compiled out of dropbear.
- `prctl` options other than the dumpable flag.
- No `scp`/`sftp` client on CosmoOS pointed at other machines (dropbear's
  `scp` is the server half here; there is no `dbclient` or `ssh` client).
