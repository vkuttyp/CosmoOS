# tools

Host-side development tools: image builders, symbolizers, debugging aids. May use languages other than C.

`keys/` holds the public keys compiled into the kernel's module key ring
and the development signing key (see `keys/README.md`); the same key
signs packages. `pkgbuild.py` builds recipes under `ports/` into signed
`.cpk` packages and a signed repository `INDEX` (`make ports`,
`docs/pkg/`).
