# Package system: architecture

Phase 10 of the roadmap ("ports, builder, repositories, pkg, signing").
Constitution section 47 (the kernel knows nothing about packages;
package management lives entirely in userland; dependency resolution,
versions, upgrades, removal, repository indexes, signatures, checksums,
rollback eventually), section 48 (declarative recipes, reproducible
builds), section 49 (reproducible builds), section 63 (`ports/`, `pkg/`,
`tools/`), invariant 8 (package management remains in userland) and
invariant 14 (privileged interfaces validate untrusted input: a package
is untrusted input).

## Where it sits

```text
   ports/<name>/port            declarative recipe: name, version, summary, depends, program, file
        │
        ▼  host, at build time (make ports)
   tools/pkgbuild.py            cross-compiles the recipe's sources with the userland toolchain,
        │                       writes <name>-<version>.cpk (a signed ustar package), then INDEX (signed)
        ▼
   out/<arch>-<build>/pkg/repo/ the repository: packages + INDEX; carried in the boot archive as repo/
        │                       (visible at /boot/repo on the target; a network transport comes later)
        ▼  target
   /sbin/pkg                    pkg update | install | remove | upgrade | list | info | verify | search
        │                       verifies signatures against /etc/pkg/keys/*.pub and checksums against INDEX,
        │                       resolves dependencies, extracts into /, records in the metadata database
        ▼
   /var/db/pkg/                 index (the verified INDEX), installed/<name>/MANIFEST (what is on disk)
   /usr/bin, /usr/share, ...    installed files
```

The kernel changed in one generic place for this phase: ramfs creates
the parent directories of nested boot archive entries (`repo/x.cpk`,
`etc/pkg/keys/cosmo-dev.pub`). `pkg` itself is an ordinary program using
the libc of Phase 9 (files, directories, rename; `spawn` is not even
needed) plus the SHA-512 and Ed25519 sources the kernel already has,
compiled a second time for user mode (they depend on nothing but
`stdint.h`, `string.h` and their own header).

## Purpose

Let software reach the machine as signed, checksummed, versioned units
with declared dependencies, built reproducibly from recipes, installed
and removed by one tool that records what it did. Section 47's list,
minus rollback, which is recorded as the next step.

## Responsibilities

- **Recipes** (`ports/<name>/port`): a small line-based format
  (`key: value`, repeatable keys) naming the package, its version,
  summary, dependencies with version constraints, the programs to
  cross-compile from sources in the port's directory, and the plain
  files to install with their modes. Sources are in the tree, so the
  build is reproducible by construction; fetching external sources with
  checksums and patches is the format's next extension (section 48's
  `source`, `checksum`, `patches`).
- **Builder** (`tools/pkgbuild.py`, host Python, driven by `make
  ports`): compiles every `program:` with the same flags and libc as
  `userland/`, lays the package out as `+MANIFEST` followed by the files
  in sorted order in a deterministic ustar archive (mode, size, SHA-512
  per file in the manifest; mtime 0, uid/gid 0), signs it with
  `scripts/modsign.py` (the module trailer, `COSMOSIG`, Ed25519 over the
  whole archive), and writes the repository `INDEX` (one stanza per
  package: name, version, summary, depends, file, size, SHA-512) with the
  same trailer. In self-test builds it also emits two deliberately bad
  fixtures (`badsig`, `badsum`) for the tests.
- **Repository**: a directory of `.cpk` files plus `INDEX`. Phase 10
  ships it inside the boot archive under `repo/` (the kernel's bootstrap
  namespace puts it at `/boot/repo`); `/etc/pkg/repos.conf` lists
  repository directories, one per line, so a mounted disk or, later, a
  fetched copy works the same way.
- **Trust**: `/etc/pkg/keys/*.pub` hold the accepted Ed25519 public keys
  (64 hex characters each, the format of `tools/keys/*.pub`); a package
  or index whose trailer names an unknown key or fails to verify is
  rejected before any byte of it is interpreted further. The development
  key signs everything in this phase; production keys are a
  configuration matter, not a code change.
- **pkg** (`pkg/`, installed as `/sbin/pkg`): `update` verifies and
  stores the index (several repositories are concatenated after each is
  verified); `install NAME[=VERSION]... | FILE.cpk` resolves the
  dependency closure from the index (newest version satisfying every
  constraint, dependencies first, cycles refused; a package file's
  missing dependencies come from the index), verifies each package
  (index checksum, signature, manifest against members, per-file
  checksums), installs files through a temporary name and `rename`,
  records the manifest;
  `remove NAME...` refuses while another installed package depends on it
  (unless `-f`), unlinks the files and removes directories it created;
  `upgrade` installs newer index versions of installed packages and
  removes files that disappeared; `list`, `info NAME`, `search TEXT`,
  `verify [NAME...]` (recompute installed file checksums).
- **Metadata database** (`/var/db/pkg/`): text files, one directory per
  installed package with its manifest and the list of directories it
  created. SQLite (section 47, "if appropriate") is not appropriate for
  a system whose libc is a week old; the layout is flat enough to
  migrate.

## Non-responsibilities

- Fetching over the network, mirrors, delta updates: the transport is a
  directory today; `repos.conf` is where a URL goes when there is a
  fetcher.
- Rollback and transactions across packages (section 47, "eventually"):
  a single package installs atomically per file and is rolled back if
  one of its files fails; a multi-package operation stops at the first
  failure with the earlier packages installed.
- Build isolation, a ports tree of third-party software, patches,
  configure/make-style builds: recipes describe programs built from
  in-tree C sources with the project toolchain; that is what exists to
  package.
- Package scripts (pre/post install hooks), users and groups, file
  capabilities, alternatives: recorded as future recipe keys.
- Verifying that a running binary came from a package (that is the
  kernel's future measured-boot/secure-exec work, not `pkg`).

## Interfaces at a glance

| Interface | Where | Used by |
|---|---|---|
| Recipe format (`name: version: summary: depends: program: file: prefix:`) | `ports/*/port`, `docs/pkg/api.md` | `tools/pkgbuild.py` |
| Package format (`.cpk`: ustar, `+MANIFEST` first, `COSMOSIG` trailer) | `docs/pkg/api.md` | builder writes, `pkg` reads |
| `INDEX` format (stanzas, `COSMOSIG` trailer) | `docs/pkg/api.md` | builder writes, `pkg update` reads |
| `pkg` command line and exit codes | `docs/pkg/api.md` | people, `/etc/rc.test`, the boot harness |
| `/etc/pkg/repos.conf`, `/etc/pkg/keys/*.pub`, `/var/db/pkg/` | `docs/pkg/api.md` | `pkg` |
| `make ports`, `PKGSIGN_KEY` | `pkg/pkg.mk` | the build, CI, `make reproducible` |

Tests (`testing.md`): a host test of `pkg`'s parsers (manifest, index,
version comparison, constraint matching, path confinement, the ustar
reader) under ASan/UBSan; `make reproducible` compares the index, two
packages and `pkg.elf`; on the target `/etc/rc.test` runs `pkg update`,
installs `fortune` (pulling `fortunes`), runs it, lists, inspects,
verifies, refuses `badsig` and `badsum`, installs `hello=1.0` then
`upgrade`s it to 1.1, removes in dependency order and refuses out of
order; the interactive harness runs `pkg install hello && hello && pkg
list` in every build.
