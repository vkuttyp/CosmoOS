# Package system: testing

Constitution section 57 (every subsystem tested where it runs) and
section 61 (formats are ABI: they are tested with fixtures, good and
bad).

| Level | What | How to run |
|---|---|---|
| Host, parsers | `tests/host/test_pkg.c` over `pkg/manifest.c`, `version.c`, `tar.c` under ASan/UBSan | `make host-test` |
| Host, crypto | the SHA-512 and Ed25519 code `pkg` links is the kernel's, covered by `test_crypto` | `make host-test` |
| Host, reproducibility | `INDEX`, `hello-1.1.cpk`, `fortune-1.0.cpk`, `pkg.elf` byte-identical across two builds | `make reproducible` |
| Target, scripted | the package section of `/etc/rc.test` (self-test builds) | `make test` |
| Target, interactive | the harness types `pkg install hello && hello && pkg list` at the prompt (every build) | `make test`, `make BUILD=release test` |
| Serial-log markers | `PKGTEST_MARKERS` in `tests/boot/run_boot_test.py` (self-test builds) | `make test` |

## `test_pkg`

Versions: parsing (`1.2.3-4`, `0`, rejections of empty, `1.`, `a.b`,
`1-`, nine components), comparison (`1.0` = `1.0.0`, `1.10` > `1.9`,
revisions, `2` > `1.99.99`). Constraints: every operator with and
without spaces, rejections (`Bad`, `x ~ 1`, `x >= `, trailing text),
satisfaction for `>=`, `=`, `<`. Names: valid and invalid. Paths: the
allowed and every forbidden shape (absolute, `..`, `.`, `//`, trailing
`/`, empty, a space, the four forbidden prefixes). Manifest: a valid
one with one dependency and one file; rejections for `../x`, a missing
version, a bad checksum, an unknown key, a missing field, a line without
a colon (the error names the line). Index: two stanzas parsed with
sizes and checksums; rejections for a `file` with `/` and a stanza
without `file`. Tar: a two-member archive built in memory (manifest and
a 600-byte file) read back with names, sizes, modes and data; a flipped
header byte is a checksum error; data cut short is an error; a 100-byte
buffer is `truncated`. Hex: decode, rejections, encode.

## The repository under test

`make ports` builds `hello` 1.0 and 1.1, `fortunes` 1.0 (a data file)
and `fortune` 1.0 (depends on `fortunes >= 1.0`); with `SELFTEST=1` the
builder adds `badsig-1.0.cpk` (a manifest byte flipped after signing) and
`badsum-1.0.cpk` (valid, but its `INDEX` stanza carries a `sha512` of
zeros). The boot archive carries them as `repo/*`, so the target sees
`/boot/repo`, and `/etc/pkg/repos.conf` names it.

## `/etc/rc.test`, package section

Straight-line shell; each failed check sets `FAILS=1`, and the script
ends with `SHTEST: PASS` or `SHTEST: FAIL n`. In order: `pkg update`;
`pkg install fortune` (pulls `fortunes` first); `fortune` runs; `pkg
list`; `pkg info fortune`; `pkg verify`; `pkg install badsig` must fail
(its stderr is shown); `pkg install badsum` must fail; `pkg install
hello=1.0`; `hello` prints `hello, world (hello 1.0)`; `pkg upgrade`;
`hello` prints `hello, world (hello 1.1)`; `pkg remove fortunes` must
fail (`fortune depends on it`); `pkg remove fortune`; `pkg remove
fortunes`; `fortune` is now not found; `pkg remove hello`; `pkg list`.

The harness requires these lines in self-test builds
(`PKGTEST_MARKERS`):

| Marker | From |
|---|---|
| `pkg: index updated: N packages` | `pkg update` |
| `pkg: installing fortunes-1.0`, `pkg: installing fortune-1.0` | the dependency closure, in order |
| `bad signature` or `unknown signing key` | `badsig` refused |
| `checksum does not match the index` | `badsum` refused |
| `hello, world (hello 1.0)`, then `hello, world (hello 1.1)` | install of a pinned version, then `upgrade` |
| `pkg: fortunes: fortune depends on it` | removal refused |
| `pkg: verify: 0 problems` | `verify` |

## The interactive harness

`tests/boot/shelltest.py` types `pkg install hello && hello && pkg list`
and requires `hello, world (hello 1.1)` and a `list` line `hello 1.1
prints a greeting`. This runs in every build, release included, so
`pkg`'s verification path is exercised without the self-tests; `hello`
1.1 stays installed when the session ends.

## Results as of Phase 10

| Configuration | Result |
|---|---|
| debug, `-smp 4` | PASS: `SHTEST: PASS` with the package section, all `PKGTEST_MARKERS`, harness complete |
| debug, `QEMU_SMP=1` | PASS |
| release | PASS (interactive `pkg install hello` only) |
| `make host-test` | `test_pkg` ok |
| `make reproducible` | `reproducible: yes` including the repository files |

## Gaps

No fuzzing of the manifest, index or tar parsers (section 60's method
applies; the host test is the place). No test injects a write failure
during installation (PK4's rollback is reviewed, not exercised). No test
of several repositories in `repos.conf`, of `--reinstall`, of `search`,
or of `install FILE.cpk` with an unmet dependency. Timing of `pkg` is
not measured.
