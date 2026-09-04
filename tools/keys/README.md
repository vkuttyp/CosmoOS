# tools/keys

Public keys compiled into the kernel's trusted key ring
(`scripts/gen-keyring.py` turns every `*.pub` here into
`out/<arch>-<build>/gen/keyring_builtin.c`), and the matching development
secret key.

| File | Content |
|---|---|
| `cosmo-dev.pub` | Ed25519 public key, 32 bytes as hex |
| `cosmo-dev.key` | Ed25519 seed, 32 bytes as hex. **Public by definition.** |

`cosmo-dev` signs every module the build produces. Because its secret
half is in the repository it authenticates nothing outside a developer's
own tree: anyone can sign a module with it. A production image must be
built with a different ring (`MODSIGN_KEY=` and a private `.pub` set
here) and the secret kept off the build host. See
`docs/kernel/module/design.md`, "Security".

Generate a new pair with:

```sh
scripts/modsign.py keygen --out-key my.key --out-pub tools/keys/my.pub
```
