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
| Host, the HTTPS client | `cosmo-fetch` against local TLS servers (`ports/mbedtls/README.md`, "Tested by") | `make host-test-fetch` |
| Target, network repositories | the installed system against repositories this harness serves over HTTPS (below) | `make BUILD=release test-pkg-net` |

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
fortunes`; `fortune` is now not found; `pkg remove hello`; then the
resolver: `pkg install demo-a demo-b` (`demolib >= 2` and `< 3`) must
pick `demolib` 2.5 (`pkg info demolib` shows `installed: 2.5`), the three
are removed, `pkg install demo-b demo-a` must pick 2.5 again
(`/usr/share/demolib/VERSION` reads `2.5`), removed again; `pkg list`.

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
| `installed: 2.5`, then `2.5` | order-independent resolution (`demo-a`/`demo-b` in both orders) |

## `test_pkg`: the index header, freshness and `repos.conf`

Time: `1970-01-01T00:00:00Z` is 0, two dates round-trip (one a leap day),
and seven malformed forms are refused (February 29 of 2026, a space for
`T`, no `Z`, month 13, hour 24, before 1970, an offset). Header: an index
with one is parsed with its serial and a 30-day validity, its packages
intact; `index_header_parse` reads the header of an index whose next line
is broken, which `index_parse` refuses naming line 5; refused are a
serial of 0, one with a leading zero, a missing `published`, a bad
`expires`, a repeated `serial`, a package key in the header and a header
key in a package stanza. `index_fresh`: accepted within the validity with
no serial before, the same serial and a lower one; refused a higher last
serial (`older than 8`), the expiry itself, a clock one second before
publication, a 31-day validity, an empty one, and no header. `repo_parse`:
the bare-directory form (`local`, then `local2`, trailing `/` dropped), an
https URL, a pin, `insecure` on http, a `file:///` URL; refused are http
without `insecure`, a bad name, `ftp://`, a short pin, a pin of another
hash, an unknown option, `insecure` on `file://`, a name alone, a relative
directory and a pin on http.

## `test-pkg-net`

`tests/boot/pkgnet_test.py` (roadmap 1.1 P6's acceptance; CI job
`pkg-net`, both architectures, `BUILD=release`). The live image installs
itself on a blank disk; the installed system boots on QEMU user
networking. The harness serves repositories over HTTPS from the host
(`tests/boot/tlstest.py`), with a test CA and certificates made for the
run; the guest appends the CA, fetched over plain HTTP, to its
`/etc/ssl/certs/ca-certificates.crt` for this boot only. Each served
`INDEX` is the build's (`make ports`, signed with `PKGSIGN_KEY`) made into
a network index by `pkgbuild.py publish`; `repos.conf` is rewritten for
each step. In order:

| Step | What | Required |
|---|---|---|
| c | `pkg update` against the same repository behind an untrusted CA's certificate, an expired one and one for another name | exit 3; `cosmo-fetch`'s `the server's certificate is not accepted:` with mbedTLS's reason; `pkg: bad: INDEX refused (above); nothing read from it`; the server logged no request |
| a | `priv -d sys_admin` `pkg update`, `search`, `info`, then `install` | the first three exit 0 (the header line `serial 10`), `install` exits 1 naming the privilege |
| a | `pkg update && pkg install hello && hello` (serial 10, hello 1.0 alone) | `pkg: fetching hello-1.0.cpk from test`, `hello, world (hello 1.0)`; the server saw `GET /repo/INDEX` then `/repo/hello-1.0.cpk` |
| f | serial 11 served (hello 1.0 and 1.1): `pkg update && pkg upgrade && hello` | `pkg: 1 package upgraded`, `hello, world (hello 1.1)` |
| d | the serial-10 index served again; then serial 12, published 40 days ago, valid 7 | each exit 3: `serial 10 is older than 11 ... (a replayed index)`, `expired at T`; `pkg info hello` still shows 1.1 available |
| b | repository `tam` whose `hello-1.1.cpk` has one byte changed: `pkg update && pkg install --reinstall hello` | exit 3, `checksum does not match the index`; `hello` still 1.1 and `pkg verify` 0 problems |
| b | repository `other`, its `INDEX` signed by a key made for the run | `pkg update` exit 3, `unknown signing key ... (not in /etc/pkg/keys)` |

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

`test-pkg-net` does not cut a transfer to exercise the resumption on the
target (`make host-test-fetch` does, on the host), nor run with the clock
before an index's publication (`test_pkg` does). No fuzzing of the manifest, index or tar parsers (section 60's method
applies; the host test is the place). No test injects a write failure
during installation (PK4's rollback is reviewed, not exercised). No test
of several repositories in `repos.conf`, of `--reinstall`, of `search`,
or of `install FILE.cpk` with an unmet dependency. Timing of `pkg` is
not measured.
