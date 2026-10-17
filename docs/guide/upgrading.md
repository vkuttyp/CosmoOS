# Upgrading

An installed system moves to a newer release with `cosmo-upgrade`,
without reinstalling, and back with `cosmo-upgrade --rollback`. It
replaces the boot set on the disk's EFI partition: loader, kernel, boot
archive, command line and manifest. The previous set is kept in
`\cosmo\prev`. Then it brings the system's programs (`/bin`, `/sbin`,
`/usr`), its `/etc` and its package repository up to the new release.
Your files, `/root`, and any `/etc` file you changed are left alone. A
changed `/etc` file gets the new release's version beside it as
`FILE.new`.

The steps below are the ones `make test-upgrade` runs (the CI job
`upgrade`, on both architectures). It installs the published v1.0.3,
upgrades it to the build under test, and rolls it back. The guide's own
runner cannot do this: it would need an older release installed first.
That is why these blocks are shown as text.

## What is checked before anything is written

Every release from v1.0.4 on publishes `SHA256SUMS.sig`, a detached
Ed25519 signature of `SHA256SUMS` with the release key. `cosmo-upgrade`
checks it against the keys in `/etc/pkg/keys` before it uses any file of
the release. These are the keys `pkg` trusts, and on a release system
they include the release key. Then it checks `kernel.elf`, `boot.tar`,
`cmdline` and `manifest` against `SHA256SUMS`. Then it checks the
manifest's own signature against the same keys, and that `kernel.elf`,
`boot.tar` and `cmdline` match the manifest. The loader is checked
through `boot.tar`, which carries it. Anything else is refused with exit
status 3, and nothing on the disk changes:

```text
cosmo-upgrade: /tmp/up: SHA256SUMS: unknown signing key ... (not in /etc/pkg/keys); nothing written
cosmo-upgrade: /tmp/up: SHA256SUMS: bad signature (key release-2026.pub); nothing written
cosmo-upgrade: /tmp/up: cosmoos-x86_64-kernel.elf does not match SHA256SUMS; nothing written
cosmo-upgrade: /tmp/up: /tmp/up/SHA256SUMS.sig: No such file or directory (a release is checked against its signed SHA256SUMS); nothing written
cosmo-upgrade: /tmp/up: manifest: bad signature (key release-2026.pub); nothing written
cosmo-upgrade: /tmp/up: boot.tar does not match the manifest (SHA-512); nothing written
```

At the next boot the new loader checks the new set against its own keys,
as it always does ([Installing](installing.md), "Verified boot"). So
`cosmo-upgrade` also refuses a release whose loader does not trust the
key that signed its own set, or the key that signed the set you are
running (the new loader starts that set if the upgrade is interrupted).
Only one `cosmo-upgrade` runs at a time; a second is refused.

`cosmo-verify FILE SUMS SIG` checks any one file of a release the same
way: `SUMS` against its signature `SIG` and `/etc/pkg/keys`, then `FILE`
against the line `SUMS` gives its name.

```text
cosmo$ cosmo-verify cosmoos-x86_64-boot.tar SHA256SUMS SHA256SUMS.sig
cosmo-verify: cosmoos-x86_64-boot.tar: OK (SHA-256 ..., in SHA256SUMS signed by key 36ae7bb4947896ab, release-2026.pub in /etc/pkg/keys)
```

On your computer, a checkout of the repository checks the same signature
with `python3 scripts/modsign.py verify-detached --pub
tools/keys/release-2026.pub --in SHA256SUMS --sig SHA256SUMS.sig`.

## Upgrading

On your computer, download the new release set for your architecture
(`kernel.elf`, `boot.tar`, `cmdline`, `manifest` and
`cosmo-upgrade.cpk`), its `SHA256SUMS` and `SHA256SUMS.sig`
([Installing](installing.md)). Serve that directory to the guest, for
example with `python3 -m http.server 8000`. QEMU's user network reaches
your computer at 10.0.2.2.

On CosmoOS, fetch the files into `/tmp`. Use `/tmp` (memory), not the
disk: v1.0.3's filesystem can lose a large file while writing it, a
defect fixed in v1.0.4.

### From v1.0.3

v1.0.3 has no `cosmo-upgrade` of its own and no `cosmo-verify`, so the
new release's must be checked before they run. They come in
`cosmoos-ARCH-cosmo-upgrade.cpk`, a package signed with the release
key. v1.0.3 shipped that key in `/etc/pkg/keys`, and its `pkg` checks a
package's signature, and every file in it, against that key.
`pkg install -n` does the check and installs nothing. Only if it prints
`pkg: installing cosmo-upgrade-...`, unpack the two programs from the
package you just checked and run the new `cosmo-upgrade`. It then checks
`SHA256SUMS.sig` and the rest of the release. (v1.0.4's package names
itself `1.0.3-123`, the build ID its release build recorded; the
signature check is what matters.)

```text
cosmo$ mkdir /tmp/up && cd /tmp/up
cosmo$ for f in kernel.elf boot.tar cmdline manifest cosmo-upgrade.cpk; do wget -q http://10.0.2.2:8000/cosmoos-x86_64-$f || break; done
cosmo$ wget -q http://10.0.2.2:8000/SHA256SUMS && wget -q http://10.0.2.2:8000/SHA256SUMS.sig
cosmo$ pkg install -n ./cosmoos-x86_64-cosmo-upgrade.cpk
pkg: installing cosmo-upgrade-1.0.3-123 (2 files)
cosmo$ tar -xf cosmoos-x86_64-cosmo-upgrade.cpk sbin/cosmo-upgrade sbin/cosmo-verify
cosmo$ ./sbin/cosmo-upgrade /tmp/up
cosmo-upgrade: verified SHA256SUMS, signed by key 36ae7bb4947896ab in /etc/pkg/keys, and the set's four files against it
cosmo-upgrade: verified kernel.elf, boot.tar and cmdline against the manifest, signed by key ... in /etc/pkg/keys, and the loader in boot.tar
cosmo-upgrade: written: the new set, its loader and \cosmo\prev's directory, in free clusters of ...
cosmo-upgrade: loader switched: \EFI\BOOT\BOOTX64.EFI is the new one
cosmo-upgrade: boot set switched: ... boots the new set; the previous one is in \cosmo\prev
cosmo-upgrade: root: ... files replaced, ... added, ... removed, ... unchanged, ... kept as changed here (the new one beside each as FILE.new)
cosmo-upgrade: upgraded; reboot to run it, then pkg update && pkg upgrade
cosmo$ exit
```

A package that is not the one the release key signed is refused, and
then nothing of it has run:

```text
pkg: ./cosmoos-x86_64-cosmo-upgrade.cpk: bad signature (key release-2026.pub)
```

### From v1.0.4 and later

The system's own `/sbin/cosmo-upgrade` came inside the signed `boot.tar`.
Fetch the set and both `SHA256SUMS` files as above (the package is not
needed) and run `cosmo-upgrade /tmp/up`.

On AArch64 the names say `aarch64` and the loader is `BOOTAA64.EFI`. Boot
again. The banner shows the new release. Then upgrade the packages from
the new repository and look at what is installed:

```text
cosmo$ pkg update && pkg upgrade
cosmo$ cosmo-upgrade --status
cosmo-upgrade: current: ..., kernel.elf ... bytes, boot.tar ... bytes, key ...
cosmo-upgrade: prev: kernel.elf ... bytes, boot.tar ... bytes, key 36ae7bb4947896ab
```

`cosmo-upgrade --check DIR` checks a release set and says whether it
would replace the current one, writing nothing.

## If it is interrupted

The partition is changed by single-sector switches. The loader's entry
switches first, then the set's directory. A power cut at any point
leaves either the old set, the new loader with the old set, or the new
set, and each of them boots. Run the same `cosmo-upgrade` again
afterwards: it finishes what the interrupted run started.

## Rolling back

```text
cosmo$ cosmo-upgrade --rollback
cosmo-upgrade: verified \cosmo\prev, signed by key ... in /etc/pkg/keys
cosmo-upgrade: boot set switched back: ... boots the previous set; the one it replaced is in \cosmo\prev
cosmo-upgrade: rolled back; reboot to run it
```

The previous boot set, its loader, its programs and its repository come
back. Packages that `pkg upgrade` changed stay changed. A package upgrade
cannot be rolled back ([Known limitations](limitations.md)).

Rolling back again moves forward again. After a rollback to v1.0.3 the
system has no `cosmo-upgrade` of its own, so fetch the newer release's
package into `/tmp/up`, check and unpack it as above, and run
`/tmp/up/sbin/cosmo-upgrade --rollback`. Its
first step makes the newer loader the one the firmware starts, since
v1.0.3's loader does not trust the newer key:

```text
cosmo-upgrade: verified \cosmo\prev, signed by key ... in /etc/pkg/keys
cosmo-upgrade: loader switched: \EFI\BOOT\BOOTX64.EFI is \cosmo\prev's, which starts both sets
cosmo-upgrade: boot set switched back: ... boots the previous set; the one it replaced is in \cosmo\prev
cosmo-upgrade: rolled back; reboot to run it
```

Next: [Privileges](privileges.md).
