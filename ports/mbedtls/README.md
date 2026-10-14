# ports/mbedtls

HTTPS for BusyBox `wget`, with the server's certificate checked (the
https unit, 2026-10-14): mbedTLS, `ssl_client` written here on it, and
the certificate authorities it trusts. BusyBox's own TLS code checks no
certificate (its `wget` prints "TLS certificate validation not
implemented" and goes on), which is why it was compiled out until now.

| Source | Version | SHA-256 |
|---|---|---|
| mbedTLS | 3.6.7 (`mbedtls-3.6.7.tar.bz2`, released 2026-07-07; the 3.6 long-term-support branch) | `a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6` (the release's `mbedtls-3.6.7-sha256sum.txt`) |
| CA certificates | curl's extract of Mozilla's, `cacert-2026-09-25.pem` (121 certificates) | `a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505` (curl's `cacert-2026-09-25.pem.sha256`) |

`scripts/fetch-pinned.py` fetches each once into `$(PORTS_CACHE)` from
github.com and curl.se, then the copy in this repository's
`pinned-sources` release, checking the digest whichever served it
(`ports/busybox/README.md`).

## The build (`mbedtls.mk`)

- **The library.** mbedTLS's own `library/Makefile`, its three static
  archives only, with BusyBox's `cc` wrapper (the musl sysroot and the
  compiler-rt builtins: `ports/busybox/README.md`). It uses the default
  configuration with `cosmo_config.h` on top (`MBEDTLS_USER_CONFIG_FILE`):
  - Entropy comes from `getrandom(2)` through `MBEDTLS_ENTROPY_HARDWARE_ALT`
    (`mbedtls_hardware_poll` in `ssl_client.c`), a strong source for every
    entropy context, PSA's own included.
  - The platform source is off (`MBEDTLS_NO_PLATFORM_ENTROPY`): on a libc
    other than glibc it reads `/dev/urandom`, which CosmoOS does not have.
  - No server side.
  - TLS 1.2 and 1.3 stay on, as in the default.
- **`ssl_client`** (`ssl_client.c`). It is linked statically with the
  three archives, then stripped.
- `-ffile-prefix-map` keeps the build directories out of both (`make
  reproducible` builds them in both trees through `MBEDTLS_DIR`).

## `ssl_client`

```
ssl_client [-e] [-k] -s FD [-r FD] [-n NAME]
```

It follows BusyBox's own `ssl_client` interface, which `wget` runs:
- TLS on socket `FD` (`-r`: a separate one to read), with plaintext on
  standard input and output.
- `-e` ends it at the end of input.
- `-k` (from `wget --no-check-certificate`) skips the check and says so on
  standard error.

`ports/busybox/wget-ssl-client.patch` makes `wget` exec it on every https
(and FTPS) connection. Unpatched BusyBox only does that on no-MMU systems
and otherwise runs its own TLS in a child.

The certificate must:
- chain to one in `$SSL_CERT_FILE`, by default
  `/etc/ssl/certs/ca-certificates.crt`;
- be valid at the wall clock's time;
- name `NAME`: a DNS name, or an address among its IP addresses.

On failure, `ssl_client` prints `ssl_client: NAME: the server's
certificate is not accepted:` followed by mbedTLS's reasons, one per line,
and exits 1. `wget` then reports `error getting response` and exits 1.

The end of the server's data ends `ssl_client`, whether or not the server
sent `close_notify`. Many HTTP servers close without it, and HTTP's own
length says whether the body is whole, as with BusyBox's `ssl_client`.

## On the system

- `/bin/ssl_client`, mode 0755.
- `/etc/ssl/certs/ca-certificates.crt`, mode 0644.

Both come from the boot archive and are copied to the installed root with
the rest of `/bin` and `/etc` (`cosmo-install`).

The bundle is the one pinned here: it changes only when this port pins a
newer one. There is no revocation check (no CRL or OCSP).

## Limits

- mbedTLS 3.6 sends the name as SNI even when it is an IP address (RFC
  6066 asks for none). The servers tested accept it.
- The validity check is only as good as the wall clock, which is set at
  boot from the RTC or firmware; there is no NTP.

## Tested by

`make BUILD=release test-net-login` (`tests/boot/net_login_test.py`, step
f) serves a 1 MiB file over HTTPS from the host with test certificates
made for the run by `openssl` (3.4 or later; no key is kept in the tree):
- With the test CA named in `SSL_CERT_FILE`, the file arrives whole.
- `wget` fails with the reason printed for each of these:
  - the installed bundle, which lacks the test CA;
  - an expired certificate;
  - a certificate for another name.
- `--no-check-certificate` fetches past the expired one.

Step c also fetches `https://example.com/` against the installed bundle,
and so does the user guide's networking page.
