# Package system: design

## Formats

### Recipe (`ports/<dir>/port`)

Line-based, `key: value`, `#` comments, keys repeatable where noted.
Paths are relative to the port directory; nothing outside it is read.

```text
name: fortune                       # [a-z0-9][a-z0-9+._-]*, at most 63 bytes
version: 1.0                        # dotted numbers, optional -N revision: 1.0, 1.2.3, 1.0-2
summary: prints a line from the fortunes database
depends: fortunes >= 1.0            # repeatable; constraint optional: =, >=, <, <=, >
prefix: /usr                        # default /usr; where program: and file: destinations live
program: bin/fortune src/fortune.c  # repeatable: destination under prefix, then C sources; mode 0755
file: share/fortunes/fortunes.txt data/fortunes.txt 0644   # repeatable: destination, source, mode
```

A recipe with neither `program:` nor `file:` is an empty package (a
meta-package that only carries dependencies). The directory name is
free; two directories may carry the same `name` with different versions
(the tests use `hello` 1.0 and 1.1).

### Package (`<name>-<version>.cpk`)

A ustar archive exactly as `scripts/mkbootarchive.py` writes them (mode
field honoured, uid/gid 0, mtime 0, no prefix field, regular files only,
names at most 100 bytes), followed by the 88-byte `COSMOSIG` trailer of
`kernel/include/kernel/modsig.h` (`sig[64] key_id[8] version=1 algo=1
magic="COSMOSIG"`), whose signature covers every byte before it.

The first entry is `+MANIFEST`; the rest are the files, paths relative
to `/` without a leading slash (`usr/bin/fortune`), sorted by path:

```text
name: fortune
version: 1.0
summary: prints a line from the fortunes database
depends: fortunes >= 1.0
file: usr/bin/fortune 0755 18432 <128 hex chars of SHA-512>
file: usr/share/doc/fortune/README 0644 220 <sha512>
```

Every `file:` line names a member that must follow, in that order; a
member not in the manifest, a manifest line without a member, a size or
checksum mismatch, a path with `..`, an absolute path, or a path that is
not under a directory `pkg` may write to (anything is allowed except
`/boot`, `/dev`, `/tmp`, `/mnt`) makes the package invalid.

### Repository `INDEX`

Stanzas separated by one blank line, sorted by name then version, with
the same trailer appended:

```text
name: fortune
version: 1.0
summary: prints a line from the fortunes database
depends: fortunes >= 1.0
file: fortune-1.0.cpk
size: 20480
sha512: <128 hex>
```

`size` and `sha512` describe the package file including its trailer.

### The metadata database (`/var/db/pkg/`)

```text
/var/db/pkg/index                       the last verified INDEX (trailer stripped)
/var/db/pkg/installed/<name>/MANIFEST   the package's manifest as installed, plus "dir: /path" lines for
                                        the directories this package created (deepest last)
/var/db/pkg/lock                        present while a mutating pkg runs
```

One file per package, staged as `MANIFEST.new` and committed by a single
`rename`, so a crash or a failure leaves either the old record or the
new one, never a mixture (`dir:` is a record-only key; the builder never
emits it and a package carrying it is not rejected but its directories
are ignored).

### Configuration

```text
/etc/pkg/repos.conf     one repository directory per line (Phase 10: /boot/repo)
/etc/pkg/keys/*.pub     accepted Ed25519 public keys, 64 hex characters (tools/keys/*.pub format)
```

## The builder (`tools/pkgbuild.py`)

```text
pkgbuild.py build --port DIR --out REPO --cc CC --cflags "..." --ld LD --ldflags "..." --ldscript user.ld
                  --crt0 crt0.o --libc libc.a --sign-key KEY [--test-fixtures]
pkgbuild.py index --repo REPO --sign-key KEY
```

`build` parses the recipe, compiles each `program:` (one `clang -c` per
source into a scratch directory under `REPO/.build/<name>-<version>/`,
one `ld.lld` with the same flags `userland.mk` uses; the build fails on
any warning because the flags carry `-Werror`), computes each file's
SHA-512, writes the manifest, assembles the archive (sorted, mtime 0),
and signs it by calling `scripts/modsign.py sign`. The output is
byte-identical across runs and hosts (`make reproducible` checks it):
the only inputs are the recipe, its sources, the toolchain flags and the
key. `--test-fixtures` additionally writes `badsig-1.0.cpk` (a copy of
`hello-1.0.cpk` with one payload byte flipped after signing: the
signature no longer matches) and `badsum-1.0.cpk` (a valid signed
package whose `INDEX` stanza carries a wrong `sha512`); their stanzas are
added by `index` when the files exist.

`index` lists `*.cpk` in the repository, reads each manifest, writes
`INDEX` sorted, and signs it. `make ports` runs `build` for every
directory under `ports/` and then `index`; the Makefile adds every file
in the repository to the boot archive as `repo/<file>`.

## `pkg` on the target (`pkg/`)

```text
pkg/
  pkg.h        limits, exit statuses, paths, the structures and the C interfaces below
  pkg.c        main, the commands, the database (/var/db/pkg records, lock) and the file helpers
               (read whole file, write through rename, mkdir -p with a record, remove empty directories)
  manifest.c   parsers: manifest and index stanzas (key: value lines), path confinement, hex
  version.c    version parsing and comparison, dependency constraints, name validation
  tar.c        ustar reader over an in-memory buffer: iterate (name, mode, size, data)
  verify.c/.h  COSMOSIG trailer: split, key ring lookup (/etc/pkg/keys), ed25519_verify; sha512 helper
  pkg.mk       builds /sbin/pkg from these plus kernel/security/{sha512,ed25519}.c; the ports target
```

The database and file helpers were planned as `db.c` and `fs.c`; they
are small enough that they live at the top of `pkg.c`.

### Reading a package

```text
load_package(path):
  blob = read whole file (limit 64 MiB)
  payload, trailer = split_trailer(blob)          (magic, version 1, algo 1)
  key = ring_lookup(trailer.key_id)               -> "unknown signing key" when absent
  ed25519_verify(trailer.sig, payload, key)       -> "bad signature"
  members = tar_next(payload)                     (bounds, checksum of each header, ustar magic, regular files,
                                                   names of at most 100 bytes; a buffer ending on a member
                                                   boundary is the end even without the two zero blocks)
  first member is "+MANIFEST" -> manifest_parse (a malformed "key: value" line is an error); else refused
  walk manifest file: lines and members in lockstep: name, size, mode, sha512 must agree; an extra member
  or a missing one is refused
```

Only after every check passes is anything written. The index checksum
(`sha512:` of the whole `.cpk`) is checked before the signature when the
package came from a repository, so a corrupted download is reported as
such rather than as a bad signature.

### Version and constraints (`version.c`)

`1.2.3-4`: dotted numeric components compared left to right (missing
components are 0), then the revision. Constraint grammar: `name`,
`name OP version` with `OP` in `= >= <= < >`; spaces optional. The
newest index version satisfying all constraints imposed by the request
and by every dependant in the closure is chosen; an installed version
that violates a constraint is replaced by the newest satisfying index
version (`pkg: NAME will be upgraded from A to B`), and the install fails
when the index has none.

### Resolution (`pkg install`)

Every constraint seen on a name is remembered in a constraint set
(`cons_add`), seeded with the `depends:` of every installed package so
that an upgrade never breaks an installed dependant. A package is
chosen as the newest index version satisfying all constraints on its
name (`index_best_all`). The result does not depend on traversal order:

```text
plan_build(requests):
    add every installed package's constraints
    repeat up to 16 times:
        plan = []; retry = false
        for req in requests: resolve(req)
        if not retry: return plan
    fail "cannot settle versions"
resolve(d):
    add d to the constraints on d.name
    if d.name is planned: if the planned version violates the constraints now known: retry = true; return
    cycle through d.name -> error
    if installed (and not --reinstall) and the installed version satisfies all constraints: return
        (installed but violating: the newest satisfying index version is planned: "will be upgraded")
    chosen = index_best_all(d.name); none -> error listing the constraints
    mark visiting; for dep in chosen.depends: resolve(dep); unmark
    plan.append(chosen)                             (dependencies first)
```

A later constraint that invalidates an earlier choice triggers a rebuild
with the enlarged constraint set (`demo-a` wants `demolib >= 2`, `demo-b`
wants `demolib < 3`: 2.5 is chosen in either order). Constraints
gathered from a candidate that a later round discards stay in the set
for that run: a slightly over-constrained plan can fail where a search
would succeed (recorded).

Then, in order: open the package file (`<repo>/<file>`), check size and
SHA-512 against the index stanza, `load_package`, install.
`pkg install NAME=VERSION` turns the argument into the constraint
`NAME = VERSION`; an argument containing `/` is a package file, whose
dependencies are resolved from the index when they are not installed.
With `-n` each step prints what it would do (`pkg: installing ...`) and
writes nothing.

### Installing one package

```text
install(pkg):
  stage the record: write installed/<name>/MANIFEST.new (a database that cannot be written stops here)
  for each file: mkdir -p its directory (recording every directory that did not exist)
                 write to "<path>.pkgtmp" with the manifest mode, rename over the destination
  restage MANIFEST.new with "dir:" lines for the directories created plus the previous version's
  commit: rename MANIFEST.new -> MANIFEST (one rename, one file)
  on any failure so far: unlink the files this package placed, remove the directories it created,
                         delete the staged record, report, stop the operation
  if a previous version was installed: unlink files of the old manifest that the new one lacks
                                       (best effort, reported), remove its recorded directories that are empty
```

The record is committed by one rename before any file of the previous
version is touched, so the database describes files that exist at every
step: a failure before the commit leaves the old record and (for a fresh
install) no files; a failure after it can only leave an obsolete file of
the old version behind, which is reported. The recorded gap remains that
the new files overwrite the old version's files with the same paths
before the commit.

A file that already exists and belongs to another installed package is
a conflict, reported before writing (`pkg` scans the installed manifests
for each path).

### Removing

`pkg remove NAME`: refuse when another installed package `depends:` on
it (list them; `-f` overrides); unlink every manifest file (a file
already missing is fine); if any file could not be removed, the record
is rewritten to list only the files still on disk (so the database keeps
describing the filesystem, the files stay owned, and a later `remove`
finishes the job) and the command fails; if that record cannot be
written either, the record is dropped and the stuck files are named as
untracked, so the database never lists files that are gone; otherwise
`rmdir` the recorded directories deepest first (non-empty ones are left)
and remove the record.

### Upgrade

For every installed package whose index has a newer version satisfying
the constraints of the other installed packages: install it (the
"previous version" path above). `pkg upgrade NAME...` restricts the set.

### Verify

Recompute SHA-512 of every installed file against the recorded manifest;
report missing and modified files; exit 1 if any.

## Ownership, memory, limits

`pkg` is a short-lived program: it reads whole files into `malloc`
memory (a package at most 64 MiB, an index at most 4 MiB), keeps parsed
stanzas in arrays (at most 1024 index entries, 64 dependencies per
package, 4096 files per package, 256 recorded directories, 128
installed packages listed), and frees what it can as it goes. Every
limit is a constant in `pkg.h` and a clear error message.

## Concurrency

None: one `pkg` at a time is assumed. A lock file (`/var/db/pkg/lock`
created with `O_EXCL`) refuses a second concurrent run; a stale lock
after a crash is removed by hand (`pkg` says so). Records are written
through `rename`, so the database is never half-written.

## Error handling

Every failure is a message on stderr in the form `pkg: <what>: <why>`
and exit status 1 (`EXIT_FAILED`; 2 `EXIT_USAGE` for usage errors, 3
`EXIT_REFUSED` for a verification failure: `not signed`, `bad signature
(key X.pub)`, `unknown signing key ID`, `checksum does not match the
index (corrupt or replaced)`, a malformed package or manifest, so
scripts can tell "refused" from "failed"). Nothing is written before a
package passed every check; a failure while writing rolls the package
back.

## Security

The boundary is the package file and the index: both are parsed with
bounds on every field, sizes are checked against the archive, checksums
against the index, signatures against the key ring, and paths are
confined (no `..`, no absolute paths, nothing under `/boot`, `/dev`,
`/tmp`, `/mnt`). Only uid 0 may install (`pkg` refuses otherwise; the
filesystem would refuse too once permissions are enforced). The key ring
directory is trusted content of the root filesystem. Replay (an old
signed index) is not prevented in this phase: there is no time source to
anchor an index date, recorded as a gap.

## Reproducibility

Packages and the index are deterministic functions of the tree and the
key: sorted members, zero timestamps, fixed ownership, the same compiler
flags as the userland (`-ffile-prefix-map`), and a signature that is
deterministic for Ed25519. `scripts/check-reproducible.sh` compares
every `pkg/repo/*` file across two builds.

## Future extensibility

- A fetcher: `repos.conf` entries with a URL, `pkg update` downloading
  `INDEX` and packages into a cache directory; the verification path is
  unchanged.
- Rollback: keep the previous version's package file in
  `/var/cache/pkg` and the previous manifest; `pkg rollback NAME`.
- Recipe keys `source:` (URL), `checksum:`, `patches:`, `build:` steps,
  and a ports tree of third-party software once a compiler runs on the
  target.
- Index freshness (a signed date once there is a clock), key rotation
  (several keys in the ring already work; revocation lists do not
  exist), delta packages.
