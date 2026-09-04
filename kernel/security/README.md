# kernel/security

Credentials, capabilities, permission checks, secure module-loading policy hooks, audit. Security boundaries never depend on convention (Invariant 13).

Phase 5 contents: `sha512.c` (FIPS 180-4), `ed25519.c` (RFC 8032
verification only, variable-time by design since every input is public),
`keyring.c` over the generated `keyring_builtin.c` (the `.pub` files in
`tools/keys/`). Credentials and capabilities are later phases.
Documentation: `docs/kernel/security/`.
