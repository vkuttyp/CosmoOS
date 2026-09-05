# Package system: invariants

Rules that must not be broken without changing this document and the
code together. Each names how it is checked today and what is not yet
covered.

**PK1. The kernel knows nothing about packages** (constitution section
47, invariant 8). `pkg` is a user program on the Phase 9 libc; the
package and index formats, the key ring, the database and the
repository are files. The only kernel change of Phase 10 is generic:
`ramfs_populate_boot` creates the parent directories of nested archive
entries. Check: `grep -r pkg kernel/` finds nothing but that ramfs
comment; `pkg/pkg.mk` links `pkg.elf` from `pkg/*.c`, the two kernel
crypto sources (pure C) and `libc.a`. Gap: review only; no build barrier
keeps a kernel header out of `pkg/` beyond `kernel/crypto.h`.

**PK2. Nothing from a package or an index is acted on before it has
been verified.** `load_package` reads the whole file, checks (when it
came from a repository) size and SHA-512 against the stored index,
verifies the trailer's signature against `/etc/pkg/keys`, parses the
manifest, and walks members and manifest lines in lockstep comparing
name, size, mode and SHA-512, before `install_loaded` writes anything;
`cmd_update` verifies an `INDEX` before storing it; `install FILE.cpk`
runs the same path without the index check. Check: `rc.test` installs
`badsig` (flipped payload byte, exit 3, `bad signature`) and `badsum`
(wrong index checksum, exit 3, `checksum does not match the index`);
`test_pkg` feeds the tar reader and the parsers corrupt input. Gap: the
tests corrupt one byte and one checksum; no fuzzing of the parsers yet.

**PK3. A package writes only where it is allowed.** Every manifest path
passes `path_allowed`: relative, no empty, `.` or `..` components,
printable ASCII, not under `boot/`, `dev/`, `tmp/`, `mnt/`; the builder
applies the same rule to destinations. A file owned by another installed
package is a conflict reported before writing. Check: `test_pkg`
(`path_allowed` cases, a manifest with `../x` rejected). Gap: ownership
is decided by scanning installed manifests, so a path written outside
`pkg` is not protected; nothing prevents a package from shadowing a
program in `/bin` with one in `/usr/bin` (the shell's `PATH` decides).

**PK4. Installation is atomic per file and rolled back per package.**
Every file is first written as `<path>.pkgtmp`; only after the record is
committed are the temporaries renamed into place, so a failure before
that leaves the previous version's files and record exactly as they
were, and a fresh install that fails leaves nothing. A failure unlinks
the temporaries, removes the directories it created and the staged
record, and the operation stops.
The database record is one file, staged as `MANIFEST.new` before the
first file is written and committed by a single rename after the last,
so the database never describes a mixture of old and new, and it is
committed before any file of a previous version is removed; an obsolete
file that cannot be removed is kept in the record. A `remove` that could
not delete every file rewrites the record to the files still present and
fails; if even that write fails the record is dropped and the files are
named as untracked; if the drop fails too, the command says the database
is unwritable (the only case where a record can be stale, and it says so). Check: review; `rc.test` sees
complete installs and removals. Gap: no test injects a write failure
mid-install or mid-remove; a rename that fails while moving the
temporaries into place (after the commit) leaves that path with the old
content, which `verify` reports and a reinstall repairs; multi-package
operations stop at the first failure with earlier packages installed.

**PK5. The database describes what is on disk.** `installed/<name>/
MANIFEST` is the manifest of the package as extracted plus `dir:` lines
for the directories the package created; `remove` unlinks exactly the manifest's
files and removes exactly the recorded directories that are empty;
`upgrade` unlinks files the new manifest lacks. Check: `rc.test`
removes `fortune` and `fortunes` and then `fortune` is `not found`;
`pkg verify` reports `0 problems` after installation. Gap: a file
deleted behind `pkg`'s back is reported by `verify` but not repaired.

**PK6. Dependencies are honoured in both directions.** `install`
computes the closure (dependencies first, newest version satisfying
every constraint seen on a name: from the request, from every dependant
in the plan and from every installed package; the plan is rebuilt when a
later constraint invalidates an earlier choice, so the result is
independent of traversal order; cycles refused); `remove` refuses a
package another installed package depends on unless `-f`. Check:
`rc.test` installs `fortune` and sees `fortunes` installed first;
`pkg install demo-a demo-b` and `demo-b demo-a` both choose `demolib`
2.5 (`>= 2` and `< 3`); `pkg remove fortunes` fails with `fortune
depends on it`; `test_pkg` covers the constraint grammar and
comparisons. Gap: constraints gathered from a candidate a later round
discards remain for that run (an over-constrained plan can fail where a
search would succeed); `-f` removal leaves dependants with an unmet
dependency by design.

**PK7. Repository and package contents are reproducible functions of
the tree and the key.** Sorted members, mtime 0, uid/gid 0, the same
compiler flags as the userland, and Ed25519's deterministic signature.
Check: `make reproducible` compares `pkg/repo/INDEX`,
`hello-1.1.cpk`, `fortune-1.0.cpk` and `userland/pkg.elf` across two
builds. Gap: only three repository files are compared.

**PK8. One `pkg` at a time.** Mutating commands take
`/var/db/pkg/lock` with `O_EXCL` and release it on exit; a second run
fails with `another pkg is running`. Check: review. Gap: a crash leaves
the lock for a person to remove; `-n` runs and queries do not lock.

**PK9. The trust root is the key ring, and only it.** A trailer whose
key id is not in `/etc/pkg/keys` is `unknown signing key`; a matching
key whose signature fails is `bad signature`; both exit 3 and nothing
is written. Check: `badsig` in `rc.test`. Gap: no revocation, no key
rotation procedure, no freshness (an old signed index replays; there is
no clock to anchor a date), and the ring directory is trusted content of
the root filesystem.

## Gaps (documented, not invariants)

- No fetcher: repositories are directories (`/boot/repo` from the boot
  archive).
- No rollback of a completed upgrade (section 47 "eventually").
- Recipes describe in-tree C sources only: no `source:` URL, `checksum:`,
  `patches:` or build steps.
- No pre/post install scripts, users, groups or file capabilities.
- `pkg` runs as uid 0 like everything else; the filesystem does not
  enforce permissions yet, so the confinement in PK3 is `pkg`'s own.
