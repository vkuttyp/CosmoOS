# Package system: API

Every interface of the package system: the recipe format, the package
and index formats, the `pkg` command line, its configuration and
database, the builder's command line, the make targets, and the C
interfaces of `pkg/pkg.h` that the host test compiles. Stability
(constitution section 52): the formats carry no version field yet; a
change to any of them is a change to this document and to
`tools/pkgbuild.py` and `pkg/` together. Nothing here is a kernel
interface (invariant 8).

## Recipe (`ports/<dir>/port`)

Line-based `key: value`, `#` starts a comment, blank lines ignored.
Parsed by `parse_recipe` in `tools/pkgbuild.py`; an unknown key, a
missing `name`, `version` or `summary`, or a malformed value fails the
build.

| Key | Repeatable | Value | Rules |
|---|---|---|---|
| `name` | no | `[a-z0-9][a-z0-9+._-]*`, at most 63 bytes | required |
| `version` | no | `N(.N)*(-N)?` | required |
| `summary` | no | free text | required |
| `prefix` | no | absolute path | default `/usr`; destinations of `program:` and `file:` live under it |
| `depends` | yes | `name` or `name OP version`, OP one of `= >= <= < >`, spaces optional | normalised to single spaces |
| `program` | yes | `destination source.c [more.c ...]` | sources relative to the port directory, no `/`-leading or `..` components; compiled with the userland toolchain, installed with mode 0755 |
| `file` | yes | `destination source [mode]` | mode octal, default 0644 |

`destination` is relative to `prefix`; the resulting path (`usr/bin/x`)
must not start with `boot/`, `dev/`, `tmp/` or `mnt/` and must fit the
100-byte ustar name field. A recipe without `program:` or `file:` is a
valid empty package. Several directories may carry the same `name` with
different versions (`ports/hello`, `ports/hello-1.0`).

## Package (`<name>-<version>.cpk`)

A ustar archive followed by an 88-byte `COSMOSIG` trailer.

| Part | Content |
|---|---|
| Members | regular files only (typeflag `'0'`); name at most 100 bytes, no prefix field; mode as installed; uid/gid 0; mtime 0; standard header checksum; `ustar\0` `00` magic |
| First member | `+MANIFEST` (mode 0644): the manifest text below |
| Following members | the files, paths relative to `/` without the leading slash, in the manifest's order, which is sorted by path |
| End | two zero blocks (the reader also accepts a buffer that ends exactly on a member boundary) |
| Trailer | `sig[64] key_id[8] version:u32=1 algo:u32=1 magic[8]="COSMOSIG"` (`kernel/include/kernel/modsig.h`); the Ed25519 signature covers every byte before the trailer; `key_id` is the first 8 bytes of SHA-512 of the public key |

Manifest text (`+MANIFEST`), `key: value` lines, parsed by
`manifest_parse`:

| Key | Repeatable | Value |
|---|---|---|
| `name` | no | as in the recipe; required |
| `version` | no | as in the recipe; required |
| `summary` | no | text (at most 127 bytes) |
| `depends` | yes | constraint as in the recipe (at most 64) |
| `file` | yes | `path mode size sha512`: path relative to `/` (at most 255 bytes, `path_allowed`), octal mode, decimal size, 128 hex digits (at most 4096 files) |

A malformed line (no `:`, oversize field, bad value) is a parse error
for the whole manifest. `pkg` cross-checks the members against the
manifest in lockstep: name, size and mode must match, each member's
SHA-512 must equal the manifest's, no member may be missing or extra.

## Repository `INDEX`

Stanzas separated by one blank line, sorted by name then version, with
the same trailer appended (the signature covers the whole text):

| Key | Value |
|---|---|
| `name`, `version`, `summary`, `depends` | as in the manifest |
| `file` | the package file name in the repository directory (no `/`, not starting with `.`) |
| `size` | byte count of the `.cpk` including its trailer (non-zero) |
| `sha512` | 128 hex digits over the whole `.cpk` |

Every stanza needs `name`, `version`, `file` and `size`. Written by
`pkgbuild.py index`, verified and stored by `pkg update`.

## `pkg` command line

```text
pkg update
pkg install [-n] [-f] [--reinstall] NAME[=VERSION]... | FILE.cpk...
pkg remove  [-n] [-f] NAME...
pkg upgrade [-n] [NAME...]
pkg list | info NAME... | search [TEXT...] | verify [NAME...]
```

Options come after the command: `-n` prints the plan without writing
(`update -n` prints the package count), `-f` removes despite dependants,
`--reinstall` ignores an installed version when resolving.

| Command | Effect | Output the tests rely on |
|---|---|---|
| `update` | For each repository in `repos.conf`: read `INDEX`, verify its trailer against the key ring, parse it; concatenate the verified indexes (a blank line between them) into `/var/db/pkg/index` | `pkg: index updated: N packages from M repository/ies` |
| `install NAME[=VERSION]...` | Plan all requests together from the stored index (`NAME=VERSION` is the constraint `NAME = VERSION`; every constraint seen on a name, including those of installed packages, must hold; the plan is rebuilt when a later constraint invalidates an earlier choice), then for each package in dependency order: find `file` in a repository, check size and SHA-512 against the index, verify the signature, cross-check the manifest, install | `pkg: installing NAME-VERSION (N files)`; `pkg: NAME will be upgraded from A to B` when a constraint needs a newer installed package; `pkg: constraints on NAME: ...` with an unsatisfiable set |
| `install FILE.cpk` | An argument containing `/` is a package file: verified without an index entry; its dependencies must be installed or resolvable from the index | as above |
| `remove NAME` | Refuse when another installed package depends on it unless `-f`; unlink the manifest's files (already missing is fine); if any could not be removed, rewrite the record to the files still on disk and fail; otherwise `rmdir` the recorded directories deepest first and drop the record | `pkg: NAME: OTHER depends on it`; `pkg: removing NAME-VERSION (N files)`; `pkg: NAME: N files could not be removed; the package stays recorded with them` |
| `upgrade [NAME...]` | For every installed package (or the named ones) with a newer index version: install it; files the new manifest lacks are unlinked, the old recorded directories are removed when empty | `pkg: N packages upgraded` |
| `list` | One line per installed package: name, version, summary | `hello  1.1  prints a greeting` (columns) |
| `info NAME` | `name:`, `installed:`, `available: V (file, size bytes)`, `summary:`, `depends:` lines, `file: /path mode size` lines | |
| `search [TEXT]` | Index entries whose name or summary contains any TEXT (all with none) | |
| `verify [NAME...]` | Recompute the SHA-512 of every installed file against the record; report `missing` and `modified` files | `pkg: verify: N problems` |

Exit status: 0 success; 1 failure (a missing file, an unsatisfiable
dependency, a conflict, a write error); 2 usage; 3 refused, that is the
input failed verification: `checksum does not match the index (corrupt
or replaced)`, `bad signature (key NAME.pub)`, `unknown signing key ID
(not in /etc/pkg/keys)`, `not signed (no COSMOSIG trailer)`, a malformed
package or manifest. Messages are `pkg: <what>: <why>` on stderr.

Before any command `pkg` creates `/var/db/pkg/installed` and loads the
key ring (no keys is a failure). `update`, `install`, `remove` and
`upgrade` without `-n` take the lock file `/var/db/pkg/lock`
(`O_EXCL`); a second `pkg` reports `another pkg is running` and exits 1;
a stale lock after a crash is removed by hand.

Resolution: the newest index version satisfying every constraint from
the request and from each dependant already in the plan; a package
already installed in a satisfying version is left alone (unless
`--reinstall`); an installed version violating a constraint is upgraded
when the index has a satisfying version, else the install fails; a
dependency cycle fails (`dependency cycle through NAME`). Installation
writes each file to `<path>.pkgtmp` and renames it; a failure unlinks
this package's files and removes the directories it created, then the
operation stops. A file owned by another installed package is a conflict
(`/path is owned by OTHER`), reported before writing.

## Configuration and database

| Path | Content |
|---|---|
| `/etc/pkg/repos.conf` | one repository directory per line; `#` comments; Phase 10 ships `/boot/repo` |
| `/etc/pkg/keys/*.pub` | accepted Ed25519 public keys, 64 hex characters each (the `tools/keys/*.pub` format, at most 16 keys); Phase 10 ships `cosmo-dev.pub` |
| `/var/db/pkg/index` | the last verified `INDEX` text (trailer stripped) |
| `/var/db/pkg/installed/<name>/MANIFEST` | the installed package's manifest, as extracted |
| `/var/db/pkg/installed/<name>/MANIFEST.new` | the staged record while an operation runs; committed by rename |
| `/var/db/pkg/lock` | present while a mutating command runs |

Limits (`pkg/pkg.h`): a package at most 64 MiB, an index at most 4 MiB,
1024 index entries, 64 dependencies and 4096 files per package, 256
recorded directories, names 63 bytes, versions 31 bytes, paths 255
bytes, 128 installed packages listed.

## The builder (`tools/pkgbuild.py`)

```text
pkgbuild.py build --port DIR --out REPO --cc CC --cflags "..." --ld LD --ldflags "..."
                  --ldscript user.ld --crt0 crt0.o --libc libc.a --sign-key KEY [--test-fixtures]
pkgbuild.py index --repo REPO --sign-key KEY
```

`build` compiles each `program:` source with `CC CFLAGS -c` into
`REPO/.build/<name>-<version>/`, links with `LD LDFLAGS -T LDSCRIPT
crt0.o objects libc.a`, computes SHA-512 per file, writes
`REPO/<name>-<version>.cpk` and prints `PKG <file> (N files, M bytes)`.
The key is a 32-byte seed as 64 hex characters (`scripts/modsign.py`
format; `pkgbuild.py` imports `modsign` for the signature and the
trailer). `--test-fixtures` writes, when the recipe is `hello` 1.0,
`badsig-1.0.cpk` (the signed package with one manifest byte flipped
after signing) and `badsum-1.0.cpk` (a copy of the valid package).
`index` reads every `*.cpk` in `REPO`, writes `INDEX` (signed) and
prints `INDEX N packages`; the `badsig` stanza is synthesised with name
`badsig`, the `badsum` stanza with name `badsum` and `sha512` of 128
zeros. Output is deterministic: sorted members, mtime 0, uid/gid 0, a
deterministic signature.

## Make targets (`pkg/pkg.mk`, `Makefile`)

| Target or variable | Effect |
|---|---|
| `pkg` | `$(OUT)/userland/pkg.elf` from `pkg/*.c` plus `kernel/security/sha512.c` and `ed25519.c` compiled with `USER_CFLAGS -I pkg` into `$(OUT)/pkgprog/` |
| `ports` | `$(PKG_REPO)/INDEX`: rebuilds the repository (`rm -rf` first) from every `ports/*/port` with `pkgbuild.py build`, then `index`; depends on the ports' sources, `libc.a`, `crt0.o`, `user.ld`, the builder, the signer and the key |
| `PKG_REPO` | `$(OUT)/pkg/repo` |
| `PKGSIGN_KEY` | the signing key, default `tools/keys/cosmo-dev.key` |
| `all` | includes `pkg` and `ports` |
| `image` | the boot archive carries `sbin/pkg`, `etc/pkg/repos.conf`, `etc/pkg/keys/cosmo-dev.pub` and every repository file as `repo/<file>` (`PKG_ARCHIVE_ENTRIES`); `SELFTEST=1` builds add the two fixtures |
| `analyze` | includes `pkg/*.c` (`PKG_ANALYZE`) |
| `host-test` | includes `test_pkg` |
| `reproducible` | compares `userland/pkg.elf`, `pkg/repo/INDEX`, `pkg/repo/hello-1.1.cpk`, `pkg/repo/fortune-1.0.cpk` |

## C interfaces (`pkg/pkg.h`)

Compiled on the host by `tests/host/test_pkg.c` (`manifest.c`,
`version.c`, `tar.c` depend only on standard headers).

| Function | Contract |
|---|---|
| `bool name_valid(const char *s)` | the recipe name grammar |
| `bool version_parse(const char *s, struct version *v)` | at most 8 dotted components below 10^9 and an optional `-rev` |
| `int version_cmp(const char *a, const char *b)` | component-wise, missing components are 0, then the revision; unparsable versions compare as strings |
| `bool depend_parse(const char *text, struct depend *d)` | `name [OP version]` |
| `bool depend_satisfied(const struct depend *d, const char *version)` | applies `d->op` (`OP_NONE` is always satisfied) |
| `const char *op_text(enum cmp_op)` | `=`, `>=`, `<=`, `<`, `>` or empty |
| `bool path_allowed(const char *path)` | relative, no empty, `.` or `..` components, printable ASCII, not under `boot/ dev/ tmp/ mnt/` |
| `bool hex_decode(const char *hex, uint8_t *out, size_t n)`, `void hex_encode(...)` | exactly `2n` hex digits; lowercase output |
| `int manifest_parse(text, len, struct manifest *m, err, errlen)` | 0 or -1 with `err`; `m->files` is `malloc`ed: `manifest_free` |
| `int index_parse(text, len, struct index *ix, err, errlen)` | stanzas separated by blank lines; `index_free` |
| `void tar_open(struct tar_reader *, buf, len)`; `int tar_next(reader, struct tar_member *, err, errlen)` | 1 a member (`name`, `mode`, `size`, `data` into the buffer), 0 at the end (two zero blocks, or the buffer ending on a boundary), -1 with `err` for a truncated header or data, a bad checksum, a non-ustar or non-regular member, a name longer than 100 bytes |

`pkg/verify.h` (target only, uses the kernel crypto sources):
`ring_load(dir)` returns the number of `.pub` keys loaded or -1;
`trailer_split` and `ring_verify` split and check a `COSMOSIG` trailer;
`verify_signed(blob, len, &payload_len, err, errlen)` does both;
`sha512_of` wraps `sha512`.
