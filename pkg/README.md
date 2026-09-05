# pkg

The binary package manager (userland only, invariant 8): `/sbin/pkg`
with `update`, `install`, `remove`, `upgrade`, `list`, `info`, `search`,
`verify`. Verifies Ed25519 signatures (`/etc/pkg/keys`) and SHA-512
checksums, resolves dependencies with version constraints, installs
through rename, records what it did under `/var/db/pkg`. Built by
`pkg/pkg.mk` from the sources here plus the kernel's SHA-512 and Ed25519
code compiled for user mode. Design: docs/pkg/.
