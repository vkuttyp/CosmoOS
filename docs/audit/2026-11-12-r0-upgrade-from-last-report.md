# R0, first PR: the upgrade from v1.0.14 tested, and 1.1.0's notes, 2026-11-12

Roadmap 1.3's R0 releases main as v1.1.0. Its acceptance begins "`test-upgrade` takes an installed v1.0.14 to the
rc". No test did: `upgrade_test.py` starts from v1.0.3 or v1.0.4, and `be_test.py`'s forward stage from v1.0.7.
This PR adds that stage and the release's notes. It changes a test harness, a make variable and documents, and no
compiled code.

## Scope and baseline

Branch `r0-v110` from main at `3b05eeb8` (#516).

## What changed

- **`tests/boot/be_test.py`, stage `last`** (`--last`, `BE_LAST_TAG ?= v1.0.14`, `BE_ONLY=last`), on a disk of its
  own, before the other stages:
  1. the published v1.0.14's live image installs itself and boots verified with the release key;
  2. the build's key is trusted, a file `/etc/r0-note` says `before`, and v1.0.14's own `/sbin/cosmo-upgrade`
     takes an environment and switches to this build's set;
  3. the build boots verified with a key that is not the release key; `cosmo-upgrade --list` says the root's base
     system is the current set's, names the environment, and does not say the record is wrong; the note is
     rewritten `after`;
  4. the build's `cosmo-upgrade --rollback` records the root's rollback and reboots;
  5. v1.0.14 boots verified with the release key and the note reads `before`.
- **`Makefile`**: `test-be` fetches `BE_LAST_TAG` and passes it. `make test-upgrade` runs `test-be`.
- **`docs/releases/v1.1.0.md`**, `docs/userland/testing.md`'s stage table, the history entry.

## Found while writing it

The first version of the stage expected `--rollback` to print "rolled back; reboot to run it", as it does with no
environment. With an environment it reboots by itself; the stage failed on both architectures waiting for a prompt
and now uses the harness's `rebooting`. The product was right.

## Validation

No matrix: nothing compiled changes (AGENTS.md, a test-only PR). `gmake ARCH=… BUILD=release test-be BE_ONLY=last`
on the Mac, at this branch's tree:

| | x86-64 | AArch64 |
|---|---|---|
| install | PASS in 12.5 s | PASS in 13.9 s |
| `last` | 46.6 s | 59.5 s |
| verdict | `test-be: PASS in 59.4s` | `test-be: PASS in 73.6s` |

The environment was `be-v1.0.14` on both. Branch CI runs the whole of `test-upgrade` (label `full-ci`).

## What this does not do

- It does not tick R0: the rc, the Parallels check and the tag remain.
- The stage does not run `pkg update && pkg upgrade` on the upgraded system, and makes no name over 47 bytes; the
  notes say a pool with such a name cannot be mounted by v1.0.14.
