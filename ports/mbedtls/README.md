# ports/mbedtls

HTTPS for BusyBox `wget`, with the server's certificate checked (the
https unit, 2026-10-14): mbedTLS, `ssl_client` written here on it, and
the certificate authorities it trusts. BusyBox's own TLS code checks no
certificate (its `wget` prints "TLS certificate validation not
implemented" and goes on), which is why it was compiled out until now.
`cosmo-fetch`, on the same library and the same checks, is the HTTPS client
`pkg` and `cosmo-upgrade` run (roadmap 1.1 P6).

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
- **`ssl_client`** (`ssl_client.c`) and **`cosmo-fetch`**
  (`cosmo-fetch.c`), each with `tls_common.c`, the certificate checks they
  share. Each is linked statically with the three archives, then stripped.
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

## `cosmo-fetch`

```
cosmo-fetch [-c] [-i] [-p sha256:HEX] [-m MAX] URL FILE
```

One file over HTTPS into `FILE`, for `pkg` and `cosmo-upgrade`, which run
it (the native libc they are built on has no resolver and no TLS):
- The certificate is checked as `ssl_client` checks it (`tls_common.c`):
  the chain to `$SSL_CERT_FILE`, the validity, the URL's host. With `-p`,
  the SHA-256 of the server's public key (its DER SubjectPublicKeyInfo)
  must also be `HEX`. All of it happens in the handshake, before the
  request is sent: a refused server receives no request.
- `http://` only with `-i` (a repository marked `insecure`); otherwise it
  is refused before any connection.
- Redirects: at most 5, and only to the same scheme, host and port.
- `-c` resumes: the bytes already in `FILE` are kept and the rest asked for
  with `Range: bytes=N-`; a server that answers 200 starts the file again,
  and a 416 for a file already whole leaves it. A 206 must be exactly the
  rest (from `N` to its announced total), or the run fails.
- `-m MAX` (a decimal number, at least 1) refuses a file larger than `MAX` bytes, by `Content-Length` or
  as a chunked body grows.
- Bodies with `Content-Length` (two different ones are refused), chunked
  (once a `Transfer-Encoding: chunked` is seen), or to the end of the
  connection (a reset there is a failure). A header line the connection
  cuts is no line. Informational answers (100, 103) before the final one
  are read past; a 101 fails. Connecting to each address and every receive
  are bounded by 30 s (the connect nonblocking, then blocking again);
  sending waits as the system's TCP does.

It neither renames nor checks `FILE`: the caller compares it with what it
expects (the index's size and SHA-512, `SHA256SUMS`) and renames it into
place. Exit statuses:

| Status | Meaning |
|---|---|
| 0 | fetched |
| 1 | failed: a name that does not resolve, a connection, a body cut short (`FILE` keeps what arrived, for `-c`) |
| 2 | usage, or not an `http://`/`https://` URL, or `-p` with `http://` |
| 3 | refused: the certificate, the pin, plain HTTP without `-i`, a redirect elsewhere (`FILE` is removed if this run made it); larger than `-m` (`FILE` is removed, a resumed one too: it is not the file asked for) |
| 4 | the server answered with an HTTP error, which is printed |

Messages are `cosmo-fetch: URL: ...` on standard error; a refused
certificate prints `cosmo-fetch: HOST: the server's certificate is not
accepted:` and mbedTLS's reasons, as `ssl_client` does.

## On the system

- `/bin/ssl_client` and `/bin/cosmo-fetch`, mode 0755.
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

`make host-test-fetch` (`tests/host/test_fetch.py`) builds `cosmo-fetch`
for the host, on the same mbedTLS release and `cosmo_config.h`, with ASan
and UBSan, and runs 36 cases against local servers
(`tests/boot/tlstest.py`) with certificates made for the run: a good
server; an untrusted CA, an expired certificate and another name, each
exit 3 with no request received; the pinned key and another one (no
request); plain HTTP without `-i` (no connection) and with it; same-origin
redirects (absolute path, relative, full URL, a query alone, a fragment alone: the same resource, so a loop),
and refused
ones to another host and from https to http; a redirect loop; 404; resuming by range, from
a server that ignores ranges and of a file already whole; `-m` by
`Content-Length` and over a chunked body; a chunked body; a body cut short
(the part kept) and its resumption; a non-HTTP URL; a name that does not
resolve; two different `Content-Length` headers, a header line cut by the
close, a 206 from another offset (each exit 1); `-p` with `http://` (exit
2, no connection); a 103 before the 200; `-m` of `garbage`, `0`, `-5` and
`12x` (each exit 2). It needs OpenSSL 3.4 or later, as the next test does.

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
