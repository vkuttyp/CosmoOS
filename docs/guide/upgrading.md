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

`cosmo-upgrade` checks the new release's manifest against the keys in
`/etc/pkg/keys`. These are the keys `pkg` trusts, and on a release
system they include the release key. It checks the signature, then
that `kernel.elf`, `boot.tar` and `cmdline` match the manifest. The
loader is checked through `boot.tar`, which carries it. Anything else is
refused with exit status 3, and nothing on the disk changes:

```text
cosmo-upgrade: /tmp/up: manifest: unknown signing key ... (not in /etc/pkg/keys); nothing written
cosmo-upgrade: /tmp/up: manifest: bad signature (key release-2026.pub); nothing written
cosmo-upgrade: /tmp/up: boot.tar does not match the manifest (SHA-512); nothing written
```

At the next boot the new loader checks the new set against its own keys,
as it always does ([Installing](installing.md), "Verified boot"). So
`cosmo-upgrade` also refuses a release whose loader does not trust the
key that signed its own set, or the key that signed the set you are
running (the new loader starts that set if the upgrade is interrupted).
Only one `cosmo-upgrade` runs at a time; a second is refused.

## Upgrading

On your computer, download the new release set for your architecture and
its `SHA256SUMS` ([Installing](installing.md)). Serve that directory to
the guest, for example with `python3 -m http.server 8000`. QEMU's user
network reaches your computer at 10.0.2.2.

On CosmoOS, fetch the files into `/tmp`, check them, and run the new
release's `cosmo-upgrade` (v1.0.3 has none of its own). Use `/tmp`
(memory), not the disk: v1.0.3's filesystem can lose a large file while
writing it, a defect fixed in later releases.

The downloaded `cosmo-upgrade` is the one file v1.0.3 cannot check with
a signature: it runs before anything verifies it, and `SHA256SUMS` only
shows the download is the file the release page lists. Get both over
HTTPS from the release page you trust. From then on the system has its
own `/sbin/cosmo-upgrade`, which came inside the signed `boot.tar`.

```text
cosmo$ mkdir /tmp/up && cd /tmp/up
cosmo$ for f in kernel.elf boot.tar cmdline manifest cosmo-upgrade; do wget -q http://10.0.2.2:8000/cosmoos-x86_64-$f || break; done
cosmo$ wget -q http://10.0.2.2:8000/SHA256SUMS && grep -e '-kernel.elf' -e '-boot.tar' -e '-cmdline' -e '-manifest' -e '-cosmo-upgrade' SHA256SUMS | grep x86_64 | sha256sum -c
cosmo$ mv cosmoos-x86_64-cosmo-upgrade cosmo-upgrade && chmod 755 cosmo-upgrade
cosmo$ ./cosmo-upgrade /tmp/up
cosmo-upgrade: verified kernel.elf, boot.tar and cmdline against the manifest, signed by key ... in /etc/pkg/keys, and the loader in boot.tar
cosmo-upgrade: written: the new set, its loader and \cosmo\prev's directory, in free clusters of ...
cosmo-upgrade: loader switched: \EFI\BOOT\BOOTX64.EFI is the new one
cosmo-upgrade: boot set switched: ... boots the new set; the previous one is in \cosmo\prev
cosmo-upgrade: root: ... files replaced, ... added, ... removed, ... unchanged, ... kept as changed here (the new one beside each as FILE.new)
cosmo-upgrade: upgraded; reboot to run it, then pkg update && pkg upgrade
cosmo$ exit
```

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
into `/tmp` as above and run `/tmp/up/cosmo-upgrade --rollback`. Its
first step makes the newer loader the one the firmware starts, since
v1.0.3's loader does not trust the newer key:

```text
cosmo-upgrade: verified \cosmo\prev, signed by key ... in /etc/pkg/keys
cosmo-upgrade: loader switched: \EFI\BOOT\BOOTX64.EFI is \cosmo\prev's, which starts both sets
cosmo-upgrade: boot set switched back: ... boots the previous set; the one it replaced is in \cosmo\prev
cosmo-upgrade: rolled back; reboot to run it
```

Next: [Privileges](privileges.md).
