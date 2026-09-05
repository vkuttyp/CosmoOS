# kernel/security

Credentials, capabilities, permission checks, secure module-loading policy hooks, audit. Security boundaries never depend on convention (Invariant 13).

Phase 5 contents: `sha512.c` (FIPS 180-4), `ed25519.c` (RFC 8032
verification only, variable-time by design since every input is public),
`keyring.c` over the generated `keyring_builtin.c` (the developer public
key from `$COSMO_KEYDIR` and the release `.pub` files in `tools/keys/`;
no private key is in the repository). Credentials live in
`kernel/process/cred.c` (`kernel/cred.h`); capabilities are a later phase.
Documentation: `docs/kernel/security/`.
