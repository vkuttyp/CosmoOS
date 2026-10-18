# A network package repository: P6 — 2026-10-18

The 1.1 roadmap's P6, chosen by the owner on 2026-10-18 with six
decisions (repository format, transport, `pkg`, `cosmo-upgrade`,
publishing, privileges). Base: main `6552cb2b` (v1.0.5 published).
Goal: `pkg` and `cosmo-upgrade` fetch over HTTPS, with the certificate
checks `wget` makes, and refuse a stale or replayed index.

Planned in three PRs: (1) the HTTPS client, (2) `pkg`'s network
repositories, freshness and `make test-pkg-net`, (3) `cosmo-upgrade
--fetch`/`--check`, the published repository and the guide.

## Before

| What | Before |
|---|---|
| HTTPS on the system | `wget` through `/bin/ssl_client` (mbedTLS 3.6.7, the https unit): chain to `/etc/ssl/certs/ca-certificates.crt`, validity, name |
| A client for the system's own programs | none: `pkg` and `cosmo-upgrade` are built on the native libc, which has sockets but no resolver and no TLS |
| Repositories | directories (`/boot/repo` live, `/var/lib/pkg/repo` installed); `repos.conf` lists paths |
| Index freshness | none: a signed index replays forever (PK9's gap) |
| A release's files | GitHub release assets, whose download URLs redirect to another host |

## PR 1: the HTTPS client

**Shape.** The decision asked for one small native client library on the
TLS code `wget` uses. The native libc `pkg` and `cosmo-upgrade` link has
no resolver and could not host mbedTLS without a port of its own, so the
client is a program on the musl side, beside `ssl_client`, and the native
side runs it: `/bin/cosmo-fetch` (`ports/mbedtls/cosmo-fetch.c`). Its
certificate checks are `ssl_client`'s, moved into `tls_common.c`, which
both now link: the chain to `$SSL_CERT_FILE` (the pinned Mozilla bundle by
default), validity, the host's name, and with `-p` a pin of the server
key's SHA-256. All are made in the handshake, before the request.

| Rule | Where |
|---|---|
| `http://` only with `-i`, refused before connecting | `main` |
| redirects to the same scheme, host and port only, at most 5 | `same_origin`, `url_resolve` |
| `-c` resumes by `Range`; 200 restarts; 416 for a whole file keeps it | `main`, `file_restart` |
| `-m` by `Content-Length` and as a chunked body grows | `too_large`, `body_put` |
| 30 s without data fails | `wait_fd` in the receive callback |
| exit 0 / 1 / 2 / 3 refused / 4 HTTP error | the table in `ports/mbedtls/README.md` |

The CA bundle was already in the image (the https unit pinned curl's
extract of Mozilla's, 2026-09-25, by SHA-256), so neither its size nor its
licence is new.

**Tests.** `make host-test-fetch` builds `cosmo-fetch` for the host with
the same mbedTLS release and `cosmo_config.h`, under ASan and UBSan, and
runs 25 cases (36 after review) against local servers (`tests/boot/tlstest.py`, shared with
`test-net-login`, which takes its certificates from it now):

- the good server;
- the untrusted CA, the expired certificate and the other name, each exit
  3 with no request received;
- the pinned key, and another key (exit 3, no request);
- plain HTTP without `-i` (no connection) and with it;
- three same-origin redirects, and refused ones to another host and from
  https to http, and a redirect loop;
- 404;
- resuming by range, from a server that ignores ranges, and a file already
  whole;
- `-m` by length and over a chunked body, and a chunked body;
- a body cut short, the part kept and resumed;
- a URL that is not http(s), and a name that does not resolve.

Two mutations checked that the cases test what they name: certificate
verification made optional failed "untrusted certificate refused" (exit
0); `same_origin` always true failed "redirect to another host refused".
On the target, `test-net-login`'s https step now also runs `cosmo-fetch`:
the 1 MiB file whole with the test CA and the expired certificate refused
with exit 3 (x86-64 release, 2026-10-18: PASS in 88.3 s).

**Validation (PR 1, 2026-10-18, on `b4a9f188`).** On x86-64 and AArch64:
`make host-test`, `make host-test-fetch`, `make fuzz`, `make analyze`;
debug `make test` at `QEMU_SMP=1`, `2` and `4` with the network harness;
`make test-smp2`, `make test-chaos`, `make test-harness-retry`; `make
BUILD=release all test`; `make BUILD=release test-net-login` (the
`cosmo-fetch` lines present on both). `make reproducible` on x86-64:
`reproducible: yes`, `mbedtls/cosmo-fetch` among the files compared.
Every step exit 0.

**Review (Qodo, 14 findings on `cosmo-fetch.c`).** Fixed: a header line
cut by the close was returned unterminated; an unchecked `lseek` on resume;
the size check's addition could wrap; a 416 for a whole file ignored `-m`; a
206 was not held to the rest of the file; two different `Content-Length`
headers, a repeated `Transfer-Encoding` and a loose `Content-Range` were
accepted; a query-only redirect replaced the path; `-p` was ignored on
http; a reset ended a body of no stated length as a success. Five host
cases cover them (30 in all). Documented instead: a size refusal removes a
resumed file too (it is not the file asked for). Codex added four more,
fixed: connecting is bounded by the 30 s too (nonblocking connect, then
blocking again: a dropped SYN would otherwise wait out the kernel's eight
retransmissions per address); a fragment-only redirect is the same
resource; `-m` is parsed strictly (`garbage` or `0` had meant no limit);
1xx answers before the final one are read past. 36 host cases. Sending
still waits as the system's TCP does. Kept: a
chunked body whose zero chunk arrived but not its final empty line is
complete (every byte arrived; every caller checks size and digest).
## PR 2: `pkg`'s network repositories and PK10

**Format.** The decision named `INDEX` with per-package SHA-256 and a
separate `INDEX.sig`. The existing index already carries per-package
SHA-512 and the COSMOSIG trailer (an Ed25519 signature over the whole
text, verified against the ring), so the network index is that one format
with a header stanza added (`serial`, `published`, `expires`); a second
signature file and a second digest would be two formats for one thing.
`tools/pkgbuild.py publish` makes it from a built index. The build's own
index stays headerless, so `make reproducible` is unaffected.

| What | Where |
|---|---|
| `repos.conf`: `NAME URL [pin sha256:HEX] [insecure]`; a directory alone is still read (`local`) | `repo_parse` (`pkg/manifest.c`) |
| `pkg update`: fetch (`userland/lib/fetch.c` runs `cosmo-fetch`), verify, the header alone checked, then parse and store under `/var/lib/pkg/repos/NAME/INDEX` | `update_repo` |
| PK10: header present, at most 30 days, the clock within it (before publication refused with a message), serial not below the stored one | `index_header_parse`, `index_fresh` |
| packages fetched into `/var/cache/pkg/NAME/`, resumed, checked by `load_package` as before | `install_entry` |
| `install`/`upgrade`/`remove` need `sys_admin`; `update`, queries and `-n` do not | `holds_priv`, `main` |

`file://` repositories are exempt from PK10: the system's own disk, and
`cosmo-upgrade --rollback` puts an older one back on purpose.

**Tests.** `test_pkg` gains the time format, the header grammar, every
rule of `index_fresh` and `repo_parse`. `make test-pkg-net`
(`tests/boot/pkgnet_test.py`, CI job `pkg-net`): the installed system
against repositories served over HTTPS by the harness. Results
(2026-10-18, release builds):

| Step | x86-64 | AArch64 |
|---|---|---|
| c. untrusted CA, expired, another name: exit 3, no request logged | PASS | PASS |
| a. update/search/info without `sys_admin`, install refused; install hello 1.0 (GET INDEX, GET the package) | PASS | PASS |
| f. serial 11: `pkg upgrade` hello 1.0 → 1.1 | PASS | PASS |
| d. serial 10 replayed, serial 12 expired: refused, stored index unchanged | PASS | PASS |
| b. a package with one byte changed, an INDEX signed by another key: refused | PASS | PASS |

Mutation: the serial rule disabled in `index_fresh`, step d failed ("a
replayed serial-10 index (exit 3)").

**Validation (PR 2, 2026-10-18, on `b0102276`).** The full matrix as for
PR 1, plus `make BUILD=release test-pkg-net`, on both architectures:
every step exit 0, `reproducible: yes`. After the rebase onto PR 1's
reviewed `cosmo-fetch` (main `bc6ea9f2`), `test-pkg-net` again on both:
PASS.

## PR 3: `cosmo-upgrade --fetch` and `--check`, and publishing

**Where the repository is served.** GitHub Pages, from a `repo` branch
the workflow force-pushes as one commit: `https://vkuttyp.github.io/CosmoOS/`.
Release assets were the other choice the decision allowed; an asset's
URL (`github.com/.../releases/download/...`) answers 302 to
`release-assets.githubusercontent.com` (checked 2026-10-18 on v1.0.5's
`SHA256SUMS`), another host, which `cosmo-fetch` refuses. The GitHub
API needs no token but is rate-limited and its asset links redirect the
same way. Pages needs no secret beyond the release key (the push uses the
job's `GITHUB_TOKEN`), and the repository is public.

| What | Where |
|---|---|
| `--fetch TAG\|URL`: `SHA256SUMS` and its signature first, verified before the set's four files are fetched (resumed, each checked); then the upgrade as for a directory | `fetch_set` (`userland/system/cosmo-upgrade.c`) |
| `--check` alone: `LATEST`'s tag against `/proc/version`'s build id | `check_latest`, `tag_parse`, `tag_newer` |
| `/etc/cosmo-release.conf`: `url`, optional `pin` | `release_conf`, `userland/etc/cosmo-release.conf` |
| the site: `repo/ARCH/` from each `boot.tar`'s `repo/`, a network `INDEX` (serial = the site's + 1); `releases/TAG/` (newest 3); `releases/LATEST` | `scripts/publish-repo.py` |
| after a release, weekly re-signing, by hand | `.github/workflows/repository.yml`, `release.yml`'s `repository` job |
| an installed system's `repos.conf`: `local` and `cosmoos https://vkuttyp.github.io/CosmoOS/repo/ARCH` | `cosmo-install.c` |
| `cosmo-upgrade.cpk` carries `bin/cosmo-fetch`, which `COSMO_FETCH` names, for a system older than v1.0.6 | `scripts/mkrelease.py`, `userland/lib/fetch.c` |

**Serial.** The decision set the serial to the release's run number. The
weekly re-signing must raise it too, and the repository workflow's runs
are numbered apart from the release workflow's, so a run number could fall
below a serial already published, which installed systems would then
refuse. The serial is the published one plus 1 instead, which rises with
every publication of either kind.

**An unreachable repository is not a failure of the update.** With the
network repository in every installed `repos.conf`, PR 2's rule (the
worst status of any repository) made `pkg update && pkg install hello`
fail on a system without the network, as `test-install` showed at once
(its boots have no route to GitHub Pages, and the repository was not yet
published): `pkg: index updated: 8 packages from 1 of 2 repositories`,
exit 1. Now a repository not reached (no network, an HTTP error) is
reported, `pkg: NAME: not reached; its last index is kept`, and the update
exits 0 when another was updated; a refusal is still exit 3.
`test-pkg-net` step f covers it.

**Not pinned.** `repos.conf` and `cosmo-release.conf` support `pin`, but
the project's entries carry none: the key behind `*.github.io` is
GitHub's to rotate, and a pin would stop every installed system's updates
on the day it changes.

**Tests.** `test_publish_repo.py` (in `make host-test`) runs the script
with a key made for the run: the first publication, a re-signing (serial
2, the same stanzas), three more releases with `--keep 3`, and four refusals.
`test-pkg-net` gains step e: `cosmo-upgrade --check` against a served
`LATEST` naming v9.9.9 (newer) and the running release (up to date);
`--check --fetch` of this build's release set over HTTPS (verified,
already current, the server asked for `SHA256SUMS`, its signature, then
the four files); a set whose `SHA256SUMS` has one digest changed refused
with nothing more fetched. PASS on x86-64 and AArch64 (2026-10-18).

**Validation (PR 3, 2026-10-18).** The full matrix on `29df1ab1`, plus
`test-pkg-net`, `test-install` and `test-upgrade`, on both architectures:
every step exit 0 (`reproducible: yes`) but `test-upgrade`, which failed on
both for one reason: v1.0.3's `pkg install -n` of the release package
printed `(3 files)` (the package now carries `cosmo-fetch`) where the test
required `(2 files)`; the test now takes 3 (2 for a published v1.0.4 or
v1.0.5 set). After that fix and the rebase onto PR 2's review fixes
(`763ef79b`), `test-pkg-net` and `test-upgrade` on both: PASS (`test-upgrade`
128.9 s x86-64, 118.0 s AArch64).
