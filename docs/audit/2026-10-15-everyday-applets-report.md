# Everyday applets: S2 — 2026-10-15

The 1.1 roadmap's S2, the second unit of the shell-completeness track.
Base: main `3891e61f` (S1 merged). Goal: the small tools a shell session
reaches for, `logger` reaching a log someone can read, and the shell's
history kept across sessions.

## Before

| What | Before |
|---|---|
| the 33 applets | not built (`CONFIG_*` off) |
| `logger`, `syslog(3)` | nothing listened on `/dev/log`: a message was lost |
| ash's history | the console shell's `HOME` was `/`: history would have gone to `/.ash_history`, and an ssh session's (`HOME=/root`) elsewhere |
| `tar` testsuite | two cases excluded because `bunzip2` was absent |

## Changes

| Area | Change | Files |
|---|---|---|
| BusyBox | the 33 applets, their features at defconfig's choice (turned on in the checked-in config, their "is not set" lines dropped, `oldconfig` filling the defaults) | `ports/busybox/{applets,busybox.config,mkconfig.py}` |
| syslogd | native service binding `/dev/log`, each datagram to `SYS_log` as `syslog: [FACILITY.LEVEL] TAG: TEXT`; no privileges, so the kernel's rate limit applies; drops counted on the next line | `userland/system/syslogd.c`, `userland/etc/svc/syslogd`, `userland/userland.mk` |
| init | the console shell's and rc's `HOME` is `/root`; `/root` (0700) made after the root is chosen | `userland/init/init.c` |
| tests | `ash.sh`'s S2 section; the history stage in `busybox_test.py`; the two `bunzip2` exclusions removed | `tests/busybox/ash.sh`, `tests/boot/busybox_test.py` |

## Tests

`test-busybox` (CI job `busybox`, both architectures):

- every new applet with known input and output (the list is in
  `docs/userland/testing.md`, "BusyBox"); `xz` and `unzip` read fixtures
  made on the host and embedded as base64, which also tests `base64 -d`;
- `pgrep -f` and `pidof` name a background `sleep`'s pid; `pkill -f` and
  `killall` end one (its status 143);
- `logger -t bbtest-PID ...` appears in `dmesg` as `syslog: [user.notice]
  bbtest-PID: ...`;
- `sh` at the console, a command unique to the run, `exit`:
  `/root/.ash_history` holds it, and a second `sh`'s `history` lists it.

## Validation

(filled in from the validation matrix run)

## Not done here (inventory §12)

- `/dev/log` is root's alone (0755 socket node, no native `chmod`).
- BusyBox's `xz` decompresses only.
- `syslogd` keeps no file: the kernel's log is the record.
