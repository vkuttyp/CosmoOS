# Security: API

Every function here is a pure computation: no allocation, no locks, no
sleeping, no I/O; safe in interrupt and panic context; the only cost is
CPU time. **ABI stability: internal** (kernel API). The on-disk formats
they implement (SHA-512, Ed25519, the key id derivation) are fixed by
their standards and by `scripts/modsign.py`, which must agree with them
byte for byte.

## `kernel/include/kernel/crypto.h`

### `void sha512_init(struct sha512_ctx *ctx)`, `void sha512_update(struct sha512_ctx *ctx, const void *data, size_t len)`, `void sha512_final(struct sha512_ctx *ctx, uint8_t out[64])`

Purpose: streaming SHA-512 (FIPS 180-4).
Inputs: a caller-owned context (state, byte count, a 128-byte block
buffer); any number of `update` calls with arbitrary lengths including
0; `final` writes the 64-byte digest and wipes the context.
Lifetime: the context is single-use after `final`; call `init` again
to reuse it. Messages up to 2^64 bytes.
Failure: none.

### `void sha512(const void *data, size_t len, uint8_t out[64])`

One-shot form of the above.

### `bool ed25519_verify(const uint8_t sig[64], const void *msg, size_t len, const uint8_t pub[32])`

Purpose: RFC 8032 Ed25519 (pure, no context, no pre-hash)
verification.
Inputs: the 64-byte signature `R || S`, the message, the 32-byte
public key.
Outputs: `true` only if `pub` decodes to a point with a canonical `y`,
`R` decodes likewise, `S < L`, and `[S]B == R + [SHA-512(R||pub||msg)
mod L]A` (checked by cross-multiplying projective coordinates, no
inversion). Everything else is `false`; there is no distinction between
"malformed" and "wrong".
Concurrency: reentrant; no shared state (all constants are `const`).
Timing: variable-time by design; every input is public. Cost under
QEMU TCG is a few milliseconds per call plus the hash of the message.
Verified against RFC 8032 section 7.1 vectors on the host
(`tests/host/test_crypto.c`).

## `kernel/include/kernel/keyring.h`

### `struct trusted_key { uint8_t id[8]; uint8_t pub[32]; const char *name; }`

One trusted Ed25519 public key. `id` is the first 8 bytes of SHA-512
over `pub`; `name` is the `.pub` file's basename (`cosmo-dev`).

### `const struct trusted_key *keyring_find(const uint8_t id[8])`

Linear scan of the generated table (`keyring_builtin[]`,
`keyring_builtin_count`, in `out/<arch>-<build>/gen/keyring_builtin.c`);
NULL if absent. The table is immutable after build.

### `unsigned keyring_count(void)`, `const struct trusted_key *keyring_entry(unsigned index)`

Count and the i-th key (NULL past the end); used for logging and the
`modsig` self-test.

### `void keyring_key_id(const uint8_t pub[32], uint8_t id[8])`

The id derivation shared with `scripts/modsign.py keyid` and
`scripts/gen-keyring.py`.

## Taint (`kernel/include/kernel/panic.h`, `kernel/core/panic.c`)

### `void kernel_taint(unsigned flag)`, `unsigned kernel_taint_flags(void)`

Purpose: record, once and for all, that the running kernel is not in
its pristine policy state. Flags: `TAINT_UNSIGNED_MODULE` (bit 0), set
by `module_load` when `CONFIG_MODULE_SIG_ENFORCE=0` let a module
without a valid trailer through. Atomic OR / atomic load; any context;
never cleared. The panic report prints `taint: 0x<flags> (unsigned
module loaded)` after the stack trace when nonzero.

## Key material (`tools/keys/`)

| File | Content | Trust |
|---|---|---|
| `cosmo-dev.pub` | 32-byte Ed25519 public key as 64 hex characters | compiled into every kernel |
| `cosmo-dev.key` | 32-byte seed as hex | **public**: it is in the repository and authenticates nothing outside a developer's own tree |

A production build replaces the ring with private `.pub` files and
signs with a key that never touches the build host (`MODSIGN_KEY=`).
Generate a pair with `scripts/modsign.py keygen`. See
`tools/keys/README.md`.

## Not yet here

Credentials (`struct cred`), capabilities, permission checks, audit,
sandboxing, resource limits: section 41 of the constitution, scheduled
after the device and filesystem phases. The module loader's
user-facing entry point (`sys_module_load`) waits for them, which is
why there is none in Phase 5.
