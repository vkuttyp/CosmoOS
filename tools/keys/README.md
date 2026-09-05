# tools/keys

Public keys the kernel trusts in addition to the developer key: release
signing keys, one `*.pub` file each (32-byte Ed25519 public key as 64 hex
characters). `scripts/gen-keyring.py` compiles every `.pub` here, plus the
current developer public key, into `out/<arch>-<build>/gen/keyring_builtin.c`.
There are none yet.

**No private key is ever committed here or anywhere in the repository.**
`scripts/check-secrets.sh` (run by `make check-tools`, so by CI before every
build) fails when a `*.key` file, an unexpected file under a `keys/`
directory, or a revoked public key is tracked; `.gitignore` refuses `*.key`.

## Development signing

`SIGNING=dev` (the default) signs modules and packages with a key pair
that `scripts/devkey.sh` generates on first use in `$COSMO_KEYDIR`
(default `$HOME/.config/cosmoos/keys`, outside the tree): `dev.key` (mode
0600) and `dev.pub`. The kernel built on that machine trusts `dev.pub`;
no other kernel does, which is exactly the trust a development key
should carry. `make reproducible` builds twice with the same key
directory, so the outputs still compare equal. CI uses an ephemeral key
directory under `/tmp` that is created in the job and never uploaded.

## Release signing

`SIGNING=release MODSIGN_KEY=/path/to/release.key KEYRING_PUBS="/path/a.pub /path/b.pub"`
signs with a key that never touches a shared build host and trusts exactly
the listed public keys. Nothing is generated. Distribute the public keys
by checking them in here; never the private half.

Generate a pair anywhere with:

```sh
scripts/modsign.py keygen --out-key /secure/place/release.key --out-pub tools/keys/release-2026.pub
```

## History: the `cosmo-dev` key is revoked

From Phase 5 until the Prompt #3 critical-fix pass, this directory held
`cosmo-dev.key`, an Ed25519 seed, next to its public key. The seed is in
the repository's history for good; rewriting history in a shared
repository does not un-publish it, and anyone with a clone from that
period has it. The remediation is therefore revocation, not removal:

- the public key `49af948ba2deb98f9f7a0500d3b1f0513302e955f3dd0fe96e00795016c73561`
  (key id `f320ceec5342b9fd`) is listed in `tools/keys/REVOKED`: a
  tracked `.pub` holding it fails `scripts/check-secrets.sh`, and
  `scripts/gen-keyring.py` refuses to compile it into any ring, even from
  an untracked stale file, so no kernel built from this tree will trust
  it again;
- every module or package signed with it is unsigned as far as any
  current kernel is concerned (`-ENOKEY` under `MODULE_SIG_ENFORCE=1`);
- the developer key model above means a leak of this kind cannot recur:
  the build has no committed key to leak.

If the repository is ever made public, or if a release key is ever
committed by mistake, the same procedure applies: revoke by key, add it
to `REVOKED_PUBS`, rotate.
