# The landing terminal, second PR: the welcome screen and the login, 2026-11-13

The owner asked for a Linux-like landing terminal (a coloured welcome with the machine's facts, the prompt
unchanged) and "I also need login screen with it" (2026-11-13), and chose: root only; the boot image logs in on its
own; root's password set at the installed system's first console boot (`cosmo-install` asking was the first choice,
given up because CI's `cee-share` refuses new userland C and userland has no CeeSafe yet). The first PR (#520) made
the screen obey colour and clear escapes; this one is the session.

## Scope and baseline

Branch `login` from main at `727b4bba` (#520 merged); it was written on #520's branch. In scope: the console's session after
`/etc/rc` (a script, `/sbin/cosmo-console`), the welcome screen (`/bin/cosmo-welcome`), `/etc/profile`,
`/etc/passwd` with `x` and `/etc/shadow`, BusyBox's `login` and `passwd`, init's choice of the session, and the
harnesses that drive an installed system's console.

## What changed

- **`/sbin/cosmo-console`**, which init runs as `/bin/sh /sbin/cosmo-console` when it exists (one line of init
  changed, no line added). On the live image (its root is not cosmofs): the welcome, then the console shell. On an
  installed system: `/etc/shadow` made (0600) and `root:x` written if the system predates it; while root has no
  password, BusyBox `passwd` asks for one; then the login screen (release, architecture, name, address) and BusyBox
  `login`, again after each logout.
- **`/etc/profile`**: on the console (no `SSH_CONNECTION`) the prompt and path the console shell always had, and the
  welcome. ssh sessions are unchanged.
- **`/bin/cosmo-welcome`**: the screen cleared, the CosmoOS logo in colour with the user and host, release, kernel
  build, uptime, installed packages, shell, console size, CPUs, memory and address beside it, and a line of commands
  to start with. Each fact is read from what any user may read; one that cannot be read is left out.
- **BusyBox**: `login` and `passwd` built (`ports/busybox/applets`, `mkconfig.py`), new passwords hashed with
  SHA-512 crypt (forced: defconfig's default is DES).
- **`/etc/passwd`** says `root:x`; the live image has no `/etc/shadow`.
- **Harnesses**: `tests/boot/install_test.py`'s `Boot` answers the password and login prompts and ends a logged-in
  boot with `kill -USR2 1`; `test-install` checks the first boot's password, the login, `/etc/shadow`'s mode and its
  `$6$` hash; `test-net-login` reads the new process tree (init, the session, the login shell); `test-upgrade` reads
  the new set's `/etc/passwd`. `tests/guide/run_guide.py` answers the prompts in a session line marked `login`, and
  the guide's installed-system sessions are marked and end with `poweroff`.
- **Documents**: userland design "The console's session", API, U17, testing; the guide's "Logging in"
  (`docs/guide/shell.md`); backlog USR-10 and BUILD-11.

## Found before the PR

| Finding | Fix |
|---|---|
| init's spawn takes a program, not a script: `'cosmo-console' rejected: bad ELF magic`, and the live boot ended | init runs `/bin/sh /sbin/cosmo-console` |
| The harness typed the first password the moment `New password:` appeared, and it was echoed into the log: BusyBox prints the prompt before it turns echo off | the harness and the guide's runner answer a password prompt half a second after it, as a person would; no log of the final runs holds the password |
| After a login, `exit` returns to the login screen, so `finish()` hung | `finish()` ends a logged-in boot with init's power off |
| `test-upgrade` rolls the base system back to v1.0.3, which has no `poweroff`, and `finish()` then hung | `kill -USR2 1`, the shell's own `kill` |
| `passwd` warns `can't lock '/etc/shadow': Invalid argument` (fcntl locks refused) | not fixed; the password is changed (backlog USR-10) |
| Regenerating `busybox.config` by its README changes two unrelated options | kept as they were (backlog BUILD-11) |

## Found in review

Greptile and GitGuardian on #521; Codex found nothing.

| Finding | Fix |
|---|---|
| The welcome wrote its facts, as root, to `/tmp/.welcome.$$`: `/tmp` is everyone's, and a link another user made under that name would be followed (onto `/etc/shadow`, say) | the facts are kept in a shell variable; the welcome writes no file |
| `docs/guide/ssh.md` still said root has no password and `root:*` | it says the console's password does not log in over ssh |
| `test-install` typed only the right password: a login that let anyone in would pass | `stage_login_guard`: a logout brings the login back, a wrong password is refused, no shell prompt until the right one. With the right password typed where the wrong one is expected, the stage fails (`no b'Login incorrect' for a wrong password refused`) |
| The harness and the guide's runner each held a literal password for their throwaway guests, which the secret scanner flagged | each is made for the run (`secrets.token_hex`); the branch was rewritten so no commit holds one |

After these fixes, on the developer's Mac, release builds: `make test-install` x86-64 PASS in 110.8 s, AArch64 PASS
in 159.1 s (the Mac was busier: a one-minute load average near 9 at the start); both reboot logs show `Login
incorrect` once and no password.

## Validation

On the other machine (`docs/development.md`, "Running the matrix": the CI image, one container of 4 CPUs and 6
GiB), 2026-11-13, both architectures one after the other, on this PR's code as it stood on #520 before that PR's
review fixes. AArch64: `matrix: PASS`. x86-64: `matrix: FAIL`, two boots out of time, both passing when run again.

| Step | x86-64 | AArch64 |
|---|---|---|
| debug, release, chaos and harness-retry builds, `make cee-check` | PASS | PASS |
| `make host-test`, `make fuzz` | PASS | PASS |
| `make analyze` | clean (15 baselined, 0 unexpected, 0 stale) | clean (15 baselined, 0 unexpected, 0 stale) |
| debug `make test`, `QEMU_SMP=1` | PASS in 244.3 s | PASS in 281.9 s |
| debug `make test`, `QEMU_SMP=2` | PASS in 235.1 s | PASS in 286.9 s |
| debug `make test`, `QEMU_SMP=4` | FAIL after 304.0 s (timed out); run again, PASS in 253.1 s | PASS in 302.6 s |
| `make test-smp2` | PASS in 235.3 s | PASS in 295.1 s |
| `make test-chaos` | PASS in 256.2 s | PASS in 311.0 s |
| `make test-harness-retry` | FAIL after 304.5 s (timed out); run again, PASS in 253.1 s | PASS in 304.1 s |
| `make BUILD=release test` | PASS in 19.8 s | PASS in 24.0 s |

The two failures were `timed out after 300s`, `missing marker /^interactive-ok$/`, with no self-test failed: the
four-CPU boots' self-tests took 242 s where the reruns of the same images, an hour later, took 196 s. The tests that
run on several CPUs were the slower ones (`docs/testing/flakes.md`, 2026-11-13); what took the host's CPUs is not
known. No budget was changed.

Also, on the developer's Mac at a one-minute load average under 16, release builds, as CI runs them:

| Test | AArch64 | x86-64 |
|---|---|---|
| `make test-install` | PASS in 58.1 s | PASS in 43.7 s |
| `make test-net-login` | PASS in 214.3 s | PASS in 98.6 s |
| `make test-upgrade` | PASS in 167.7 s | PASS in 135.8 s |
| `make test-display` | PASS in 24.5 s | PASS in 31.1 s |

After the rebase onto main at `727b4bba` (#520 with its review fixes: blank cells take the colours being written),
on the same Mac, release builds: `make test-install` x86-64 PASS in 61.7 s, AArch64 PASS in 56.3 s; `make
test-display` x86-64 PASS in 32.9 s, AArch64 PASS in 22.2 s. The matrix was not run again on the rebased tree.

The welcome script ran on the owner's Parallels VM (v1.1.0, before #520): the logo and facts lined up, the escapes
drawn as text by that kernel, as expected.

## Limits

- The user guide's commands (`tests/guide/run_guide.py`, CI's guide jobs) were not run locally: they need a release
  set; CI runs them.
- The serial port is the same console: whoever is at it sees the login screen too.
- The welcome shows on the console only, not over ssh.
- A logout returns to the login screen; the boot ends with `poweroff`, as the guide now says.
