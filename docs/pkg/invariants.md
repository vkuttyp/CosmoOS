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
Each file is written to `<path>.pkgtmp` and renamed into place; a
failure unlinks the files this package has already placed and removes
the directories it created, and the operation stops. Database records
are written through rename too, so a crash leaves the old or the new
record. Check: review; `rc.test` sees complete installs. Gap: no test
injects a write failure mid-install; a failure while replacing a
previous version leaves that version's overlapping files gone (a
recorded limit of the single-package rollback); multi-package
operations stop at the first failure with earlier packages installed.

**PK5. The database describes what is on disk.** `installed/<name>/
MANIFEST` is the manifest of the package as extracted; `DIRS` the
directories the package created; `remove` unlinks exactly the manifest's
files and removes exactly the recorded directories that are empty;
`upgrade` unlinks files the new manifest lacks. Check: `rc.test`
removes `fortune` and `fortunes` and then `fortune` is `not found`;
`pkg verify` reports `0 problems` after installation. Gap: a file
deleted behind `pkg`'s back is reported by `verify` but not repaired.

**PK6. Dependencies are honoured in both directions.** `install`
computes the closure (dependencies first, newest satisfying version,
constraints from the request and from every dependant in the plan,
cycles refused); `remove` refuses a package another installed package
depends on unless `-f`. Check: `rc.test` installs `fortune` and sees
`fortunes` installed first; `pkg remove fortunes` fails with `fortune
depends on it`; `test_pkg` covers the constraint grammar and
comparisons. Gap: constraints are checked against the plan and the
installed set, not against dependants of packages being upgraded that
are not themselves in the plan (an upgrade may leave another package's
`<` constraint violated; the resolver reports only what it visits).

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
