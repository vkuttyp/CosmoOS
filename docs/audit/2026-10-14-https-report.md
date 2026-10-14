# HTTPS for wget — 2026-10-14

The fourth of the owner's post-1.0 units (after the everyday commands,
`ping`, and `scp`/`sftp`): `wget` fetches `https://` URLs with the
server's certificate checked. Base: main `7977f092` (`scp`/`sftp`, PR
#383).

## Why it was off

BusyBox `wget` was built without https since M4 (`ports/busybox/mkconfig.py`,
the guide's "HTTP only" paragraph). BusyBox 1.37.0 has three ways to do TLS:

| Path | What it checks |
|---|---|
| its own TLS (`networking/tls.c`), run in a forked child on an MMU build | nothing: `spawn_ssl_client` prints "TLS certificate validation not implemented" and turns `--no-check-certificate` on |
| `ssl_client` exec'd with `-s3 -n NAME [-e]`, the same code | the same, and only on no-MMU builds |
| `openssl s_client` (`FEATURE_WGET_OPENSSL`) | OpenSSL's chain, host and validity checks; CosmoOS has no OpenSSL, and the helper's standard error goes to `/dev/null` |

## Changes

| Area | Change | Files |
|---|---|---|
| TLS library | a new pinned port: mbedTLS 3.6.7 (the 3.6 LTS branch), its three static archives built by its own `library/Makefile` with BusyBox's `cc` wrapper; the default configuration plus `cosmo_config.h`: entropy from `getrandom(2)` as a strong "hardware" source (the platform source reads `/dev/urandom` on a non-glibc libc, and CosmoOS has none), no server side | `ports/mbedtls/mbedtls.mk`, `cosmo_config.h` |
| the helper | `ssl_client`, written here on mbedTLS, with BusyBox's helper interface (`-s FD [-r FD] -n NAME [-e]`) plus `-k`. It requires the chain to end in `$SSL_CERT_FILE` (default `/etc/ssl/certs/ca-certificates.crt`), the name (DNS or IP), and validity at the wall clock. A refusal prints mbedTLS's reasons and exits 1. `/bin/ssl_client` | `ports/mbedtls/ssl_client.c` |
| CA bundle | curl's extract of Mozilla's roots, `cacert-2026-09-25.pem` (121 certificates), pinned by SHA-256, installed as `/etc/ssl/certs/ca-certificates.crt` | `ports/mbedtls/mbedtls.mk` |
| wget | `wget-ssl-client.patch`, the BusyBox port's first patch: `wget` execs `ssl_client` on every https and FTPS connection (MMU builds included), passes `-k` for `--no-check-certificate`, and lists that option in its usage; `FEATURE_WGET_HTTPS` on (it selects `TLS`, which nothing calls after the patch) | `ports/busybox/` |
| release and CI | the port in the CI cache keys, `check-reproducible.sh` (`ssl_client` compared), the release notes' pinned-source table; the two new archives, and OpenSSH's (missed by the scp-sftp unit), attached to the `pinned-sources` release | `.github/workflows/ci.yml`, `scripts/` |
| test certificates | made by the test for each run with `openssl` (3.4 or later, for `x509 -not_after`: the CI image's, pulled in by `ca-certificates`; Homebrew's on macOS), a test CA and three server certificates, so no private key is in the tree. The first push checked them in, and GitGuardian failed the PR on the keys; the branch was rewritten without them | `tests/boot/net_login_test.py` |

## Tests

- `tests/boot/net_login_test.py` (`make BUILD=release test-net-login`, CI
  on both architectures). Step f runs three HTTPS servers on the host,
  each with one of the test certificates; the guest fetches the test CA
  over http.
  - With `SSL_CERT_FILE` naming the test CA, 1 MiB arrives and its
    SHA-256 matches.
  - `wget` fails, and `ssl_client` gives the reason, for each of these:
    - the installed bundle: "not correctly signed by the trusted CA";
    - an expired certificate: "validity has expired";
    - a certificate for another name: "Common Name (CN) does not match".
  - `--no-check-certificate` fetches past the expired one and says it did
    not check.
  - Step c also fetches `https://example.com/` against the installed
    bundle.
- The user guide's networking page fetches `https://example.com/` (CI
  runs the guide on Linux and macOS).

## Validation

- **Matrix**, local, at `32caa879`: 38 of 38 steps pass on x86-64 and
  AArch64 (host tests, fuzz, analyze, debug boots at 1, 2 and 4 CPUs with
  the network harness passing in each, `test-smp2`, chaos, harness retry,
  crash, wxn, guard, entropy, security, release boot, install, platforms,
  gic, and `make BUILD=release reproducible`, where `mbedtls/ssl_client`
  is the same in both trees).
- **`make BUILD=release test-net-login`** passes on both architectures,
  steps c and f included.

## Not done here

- The bundle is fixed at build time; nothing updates it, and there is no
  revocation check (CRL, OCSP).
- mbedTLS 3.6 sends an IP address as SNI (RFC 6066 asks for none); the
  test's server and example.com accept it.
- No `curl`, and no TLS for other programs: `ssl_client` serves `wget`.
- The guest's wall clock can be a little behind the host's: a test
  certificate valid from the moment it was made was refused as "validity
  starts in the future" on x86-64, so the test's certificates are valid
  from a day before (as CAs backdate). Not measured further here.
- The wall clock has no NTP (limitations.md). A clock far off fails every
  validity check.
