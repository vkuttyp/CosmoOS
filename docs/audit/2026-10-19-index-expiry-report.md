# The repository's expiry, and an installed system's /tmp — 2026-10-19

The project owner's task of 2026-10-19, its PR 1 (a blocker before P3
and P4): the published index must not expire under every installed
system when the weekly job stops, and an installed system's `/tmp` is
not writable by any uid but 0 (inventory section 18, both lines). Base:
main `f7107c2c` (v1.0.6). Branch `index-expiry`.

## The expiry

GitHub disables a scheduled workflow after 60 days without activity in
the repository. The published `INDEX` was signed for 30 days and renewed
by that schedule (`.github/workflows/repository.yml`), so a quiet
repository would have expired every installed system's index about 90
days after its last push, with nothing to warn anyone first.

PK10's bound becomes 90 days (`PKG_INDEX_MAX_DAYS`, `pkgbuild.MAX_DAYS`).
v1.0.6's `pkg` refuses a validity over 30 days (`index_fresh`: `valid from
T to T, not for at most 30 days`), so a plain switch would have refused
the published index on every v1.0.6 system until it upgraded. The owner
chose two signings of one index, one serial:

| File | Validity | Read by |
|---|---|---|
| `repo/ARCH/INDEX.90` | 90 days | `pkg` from v1.0.7, first |
| `repo/ARCH/INDEX` | 30 days | `pkg` before v1.0.7; `pkg` from v1.0.7 after an HTTP error for `INDEX.90` (a publisher before v1.0.7, `pkg: NAME: no INDEX.90; reading INDEX`) |

Both are checked alike (PK10), so the fallback gives a server nothing it
could not have by serving the 30-day signing as `INDEX.90`.
`scripts/publish-repo.py` writes both on every publication and every
signing again; the release workflow's `repository` job now runs for a
pre-release too, with an empty tag (sign again, publish nothing new), so
every tag renews both expiries. `docs/development.md` gains "The
repository's expiry" and a release checklist whose first step dispatches
the job (and re-enables the schedule) after a quiet month.

An expired index is refused with one sentence, exit 3, the stored index
kept:

```
pkg: test: the index (serial 12) expired at 2026-09-16T05:56:53Z, so the repository must be signed again by its publisher before pkg update accepts it; the stored index is unchanged
```

(`index_fresh` returns -2 for an expiry; `update_repo` words it.)

**v1.0.6 systems** need no action: they keep reading the 30-day `INDEX`,
which the weekly job, every release and every pre-release still sign. They
keep the 60-day schedule risk until they move to v1.0.7, whose `pkg`
reads `INDEX.90`.

## /tmp

init mounts a ramfs on `/tmp` after switching to the disk root, and a
ramfs's root is made 0755. The native interface has no `chmod`; the first
version of this PR ran `chmod 1777 /tmp` from `/etc/rc`, which Qodo's
review showed an upgraded system with an edited `/etc/rc` would never get
(`cosmo-upgrade` keeps an operator's `/etc` files and writes `.new`
beside them). The mount itself now makes the mode: a native flag,
`COSMO_MOUNT_SCRATCH`, makes a ramfs root 01777 (`MOUNT_SCRATCH` to
`ramfs_mount`; any other filesystem is `EINVAL`), and init passes it. init
travels in the boot archive, which every upgrade replaces. The live
root's `/tmp` is 01777 already (`ramfs_populate_boot`).

## Tests

- `test_pkg` (`make host-test`): `index_fresh` returns -2 at the expiry; a
  90-day validity is accepted, 91 days refused (`at most 90 days`).
- `test_publish_repo`: the first publication's `INDEX.90` expires 90 days
  after it (`2027-01-16T12:34:56Z`), is signed by the release key and
  lists what `INDEX` lists; signing again gives both the next serial.
- `test-pkg-net` (both architectures): the harness serves both signings
  as the publisher makes them; the first install fetches `INDEX.90`
  (expiring 90 days after its publication) then the package; the expired
  index's refusal is the sentence above with the right time, the stored
  index still serial 11; a server with `INDEX` alone is read after
  `INDEX.90`'s 404.
- `test-install` (both architectures): on the installed system `stat -c
  '%a %u' /tmp` prints `1777 0` and `priv -u 1000 -g 1000 touch
  /tmp/u1000` makes a file owned by 1000.
- init's self-test (every debug boot): a `SCRATCH` ramfs is 01777 and
  root's, a plain one 0755, and `SCRATCH` with `procfs` is `EINVAL`.
- `tools/tmp-mode-probe.py [--old] [--arch A]`: `--old` reverse-applies
  the fix commit's kernel and init changes in a throwaway worktree;
  test-install's reboot stage then fails `uid 1000 writing in the
  installed system's /tmp`.

| Probe | x86-64 | AArch64 |
|---|---|---|
| `tmp-mode-probe.py --old` | PASS: uid 1000 could not write in /tmp | PASS: the same |
| `tmp-mode-probe.py` | PASS: test-install PASS | PASS: test-install PASS |

## Validation

The device-lifecycle matrix at `b43635bc`, both architectures, every step
exit 0 (seconds):

| Step | x86-64 | AArch64 |
|---|---|---|
| `host-test`, `fuzz`, `analyze` | 81, 35, 218 | 82, 35, 210 |
| debug `test` at `QEMU_SMP=1`, `2`, `4` (network harness in each) | 270, 153, 158 | 257, 163, 163 |
| `test-smp2`, `test-chaos`, `test-harness-retry` | 167, 188, 182 | 172, 198, 189 |
| `BUILD=release all test` | 50 | 55 |
| `BUILD=release test-install`, `test-pkg-net` | 29, 63 | 47, 88 |

`test-pkg-net` step g read the real published repository, which has no
`INDEX.90` yet: `pkg` fell back to `INDEX` (serial 3, expiring
2026-11-18T05:34:08Z). The first publication after this merges adds
`INDEX.90`.
