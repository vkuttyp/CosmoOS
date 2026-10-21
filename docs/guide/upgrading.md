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

## A root raised to format 12

A snapshot of the root raises its filesystem to format 12
([Snapshots and rollback](snapshots.md)), which a kernel of v1.0.7 or
older cannot mount. `cosmo-upgrade` reads the newest format each
release's kernel can mount from the kernel itself, and refuses to switch
to one -- an upgrade to an older release, or `--rollback` to the set in
`\cosmo\prev` -- that could not mount the root:

```text
cosmo-upgrade: \cosmo\prev's kernel reads cosmofs format 11 at most, and the root is format 12 (a snapshot raised it): that kernel could not mount the root; nothing written
```

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

## Fetching directly (from v1.0.6)

From v1.0.6 `cosmo-upgrade` fetches a release itself, over HTTPS, from
the project's release index on GitHub Pages,
`https://vkuttyp.github.io/CosmoOS/releases` (`/etc/cosmo-release.conf`
names it; the release workflow publishes each release there, without its
images). The certificate is checked as `wget` checks it, against
`/etc/ssl/certs/ca-certificates.crt`, so the clock must be right.

```text
cosmo$ cosmo-upgrade --check
cosmo-upgrade: v1.0.7 is newer than this system's v1.0.6 (published ...): cosmo-upgrade --fetch v1.0.7
cosmo$ cosmo-upgrade --fetch v1.0.7
cosmo-upgrade: fetching the x86_64 set from https://vkuttyp.github.io/CosmoOS/releases/v1.0.7 into /var/cache/cosmo-upgrade/v1.0.7
cosmo-upgrade: verified SHA256SUMS, signed by key 36ae7bb4947896ab in /etc/pkg/keys
cosmo-upgrade: fetched the set's four files, each matching SHA256SUMS
cosmo-upgrade: verified SHA256SUMS, signed by key 36ae7bb4947896ab in /etc/pkg/keys, and the set's four files against it
...
cosmo-upgrade: upgraded; reboot to run it, then pkg update && pkg upgrade
```

`SHA256SUMS` and its signature are fetched and checked first; a release
they do not cover is refused before anything else is downloaded. Then
everything is checked again, and written, as for a directory (below).
`--fetch` also takes a URL, `https://.../DIR`, of a directory holding the
set under the release's names; `--check --fetch` fetches and checks
without writing. A transfer that is cut resumes where it stopped when
the command runs again.

**From v1.0.6 to v1.0.7 or later, run `--fetch` twice.** The running
`cosmo-upgrade` brings the root to the new release, and v1.0.6's does not
know `/lib`, which v1.0.7 adds (`libc.so` and the dynamic linker,
`ld-musl-ARCH.so.1`): the first run installs everything else, the new
`cosmo-upgrade` among it. The second run is the new one; it finds the
set already current and finishes, adding `/lib`:

```text
cosmo$ cosmo-upgrade --fetch v1.0.7
...
cosmo-upgrade: upgraded; reboot to run it, then pkg update && pkg upgrade
cosmo$ cosmo-upgrade --fetch v1.0.7
...
cosmo-upgrade: the boot set on nvme0n1 is already this one: finishing
cosmo-upgrade: upgraded; reboot to run it, then pkg update && pkg upgrade
```

Without the second run the system works, but `/bin/busybox.dyn` and the
other dynamically linked programs fail to start (no interpreter).

**From v1.0.5 and earlier**, the system has no `cosmo-fetch` and its
`cosmo-upgrade` cannot fetch. Fetch the new release's package with
`wget`, check it, and run the programs it carries; `COSMO_FETCH` tells
the new `cosmo-upgrade` where its `cosmo-fetch` is:

```text
cosmo$ mkdir /tmp/up && cd /tmp/up
cosmo$ wget -q https://vkuttyp.github.io/CosmoOS/releases/v1.0.6/cosmoos-x86_64-cosmo-upgrade.cpk
cosmo$ pkg install -n ./cosmoos-x86_64-cosmo-upgrade.cpk
pkg: installing cosmo-upgrade-1.0.6 (3 files)
cosmo$ tar -xf cosmoos-x86_64-cosmo-upgrade.cpk sbin/cosmo-upgrade bin/cosmo-fetch
cosmo$ COSMO_FETCH=/tmp/up/bin/cosmo-fetch ./sbin/cosmo-upgrade --fetch v1.0.6
```

After the reboot the system has its own `cosmo-fetch` and `--fetch`. Its
`/etc/pkg/repos.conf` is still the one the older installer wrote, naming
the disk's repository alone (an `/etc` file the system has kept is never
replaced; the new one is beside it as `repos.conf.new`). Add the network
repository to it, for your architecture:

```text
cosmo$ echo 'cosmoos https://vkuttyp.github.io/CosmoOS/repo/x86_64' >> /etc/pkg/repos.conf
cosmo$ pkg update
```

On AArch64 the line ends in `repo/aarch64`.

## Upgrading from a directory

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
itself `1.0.3-123`, the build ID its release build recorded; from v1.0.5
on the package and the kernel's banner name the release. The signature
check is what matters.)

```text
cosmo$ mkdir /tmp/up && cd /tmp/up
cosmo$ for f in kernel.elf boot.tar cmdline manifest cosmo-upgrade.cpk; do wget -q http://10.0.2.2:8000/cosmoos-x86_64-$f || break; done
cosmo$ wget -q http://10.0.2.2:8000/SHA256SUMS && wget -q http://10.0.2.2:8000/SHA256SUMS.sig
cosmo$ pkg install -n ./cosmoos-x86_64-cosmo-upgrade.cpk
pkg: installing cosmo-upgrade-... (3 files; 2 before v1.0.6)
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
would replace the current one, writing nothing; `cosmo-upgrade --check`
alone says whether a newer release is published.

## If it is interrupted

The partition is changed by single-sector switches. The loader's entry
switches first, then the set's directory. A power cut at any point
leaves either the old set, the new loader with the old set, or the new
set, and each of them boots. Run the same `cosmo-upgrade` again
afterwards: it finishes what the interrupted run started.

## Boot environments

From v1.0.8 on, an upgrade can be fully undone. Before it writes
anything, `cosmo-upgrade` takes a snapshot of the root -- a **boot
environment**, `be-<release>` -- and pairs it with the boot set it ran on:

```text
cosmo$ cosmo-upgrade /tmp/up
...
cosmo-upgrade: boot environment be-v1.0.8: the root as it is now, paired with the current set
...
cosmo-upgrade: upgraded; reboot to run it, then pkg update && pkg upgrade; --rollback returns to be-v1.0.8
cosmo$ cosmo-upgrade --list
ENVIRONMENT              SET       SET-ID        SNAPSHOT
be-v1.0.8                previous  3f2a9c0d1e5b  on the root
cosmo-upgrade: the root's base system is the current set's (v1.0.9, 7c41e2b09a5d)
```

The two newest environments are kept; `cosmo-upgrade --prune` deletes any
older ones. An upgrade refuses to start, writing nothing, when the root
lacks the room for one (`not enough space on the root for the boot
environment: about N KiB needed ..., M KiB free`), while a rollback of the
root is pending, and when the root's programs are the previous set's, not
the current set's (a rollback left half done: `cosmo-upgrade --repair`
first). What the root holds is told from its files -- `/bin`, `/sbin`,
`/usr`, `/lib` and the package repository against each kept set's boot
archive -- not from the record `cosmo-upgrade` keeps of it
(`/var/lib/cosmo-upgrade/root-set`), which only follows the files.

### After rolling back and forward on v1.0.8

On v1.0.8, `--rollback` to v1.0.7 and then v1.0.7's own `cosmo-upgrade
--rollback` forward again leave that record naming v1.0.7's set: v1.0.7's
tool does not keep it. v1.0.8's own `cosmo-upgrade` trusts the record,
so its next upgrade is refused, and its `--repair` finds nothing to do:

```text
cosmo-upgrade: the root holds another set's base system than the current set's (a rollback half done): cosmo-upgrade --repair first; nothing written
```

Run v1.0.9's `--repair` once, from the release's signed package (as for
v1.0.5 above; it checks against the release key v1.0.8 ships), then
upgrade as usual. It corrects the record from the root's files and
changes nothing else:

```text
cosmo$ mkdir /tmp/up && cd /tmp/up
cosmo$ wget -q https://vkuttyp.github.io/CosmoOS/releases/v1.0.9/cosmoos-x86_64-cosmo-upgrade.cpk
cosmo$ pkg install -n ./cosmoos-x86_64-cosmo-upgrade.cpk
pkg: installing cosmo-upgrade-1.0.9 (3 files)
cosmo$ tar -xf cosmoos-x86_64-cosmo-upgrade.cpk sbin/cosmo-upgrade
cosmo$ ./sbin/cosmo-upgrade --repair
cosmo-upgrade: the root's record named the previous set's (v1.0.7, 1a2b3c4d5e6f) base system, but its files are the current set's (v1.0.8, 3f2a9c0d1e5b): record corrected
cosmo-upgrade: no boot environments recorded: nothing more to repair
cosmo$ cd / && cosmo-upgrade --fetch v1.0.9
```

From v1.0.9 on, `cosmo-upgrade --list` says when the record is wrong,
naming the set it names and the set the files are; the upgrade corrects
it by itself.

The upgrade *to* v1.0.8 takes no environment: v1.0.7 and older keep no
snapshots, and their `cosmo-upgrade` would refuse a system partition
that records environments once rolled back to. The first upgrade that can
be fully undone is the one made from v1.0.8. Rolling back past an upgrade
without an environment returns the set and the base system only, as
below. For the same reason no environment is taken when its snapshot
would raise the root to cosmofs format 12 and a kept set's kernel reads
format 11 at most (`no boot environment: ...`).

## Rolling back

With a boot environment for the previous set, `--rollback` returns both
halves -- the boot set now, and the root at the next boot:

```text
cosmo$ cosmo-upgrade --rollback
cosmo-upgrade: verified \cosmo\prev, signed by key ... in /etc/pkg/keys
cosmo-upgrade: intent recorded: \cosmo\be names the previous set and be-v1.0.8
cosmo-upgrade: the root rolls back to be-v1.0.8 at the next boot; the tree it replaces is kept as be-v1.0.9
cosmo-upgrade: boot set switched back: ... boots the previous set; the one it replaced is in \cosmo\prev
cosmo-upgrade: rolled back: the boot set now, and the root -- packages, /etc and the base system as they were in be-v1.0.8 -- at the next boot
cosmo-upgrade: rebooting to apply it
```

It reboots at once, as `cosmo-snap rollback` does: the tree it keeps is
fixed when the root's rollback is recorded, so what is written after that
would be in neither (`--no-reboot` leaves the reboot to you, and what is
written before it is lost). After the reboot the packages, `/etc` and the base system are exactly
what they were when the environment was taken. Anything else written
since is gone from the live root -- with one exception: files added or
changed under `/root`, `/home` and `/var/log` are copied, before any
service starts, into `/var/lib/cosmo/rollback/<the replaced tree's
name>/`:

```text
cosmo-upgrade: 3 file(s) written under /root, /home and /var/log after be-v1.0.8 was taken are kept in /var/lib/cosmo/rollback/be-v1.0.9
```

If one cannot be copied (no room under `/var`, for one), the boot says
so, the files stay in the replaced tree, and that tree stays held --
undeletable, and the only rollback allowed -- until a boot copies them
all; make the room and reboot.

The whole replaced tree also stays, as the environment `be-v1.0.9`:
rolling back again goes forward to it.

If the machine stops in the middle, the system still boots -- possibly
the newer kernel on the older root, or the older kernel on the newer one
-- and `cosmo-upgrade --repair` finishes what was meant, which
`\cosmo\be` records before anything moves (it reboots when it asked for
the root's rollback; otherwise reboot after it). It also finishes an
upgrade whose base pass was cut short. If `\cosmo\be` itself is damaged,
the commands that need it refuse, and `cosmo-upgrade --prune` writes it
again, empty, keeping every `be-*` snapshot (`cosmo-snap list --all`).
`cosmo-upgrade --status` says when a repair is needed, and when a rollback
of the root is pending but the last boot did not apply it (init says so
on the console too).

Without a boot environment for the previous set -- it was installed by
v1.0.7 or older -- the previous boot set, its loader, its programs and its
repository come back, and packages that `pkg upgrade` changed stay
changed:

```text
cosmo$ cosmo-upgrade --rollback
cosmo-upgrade: verified \cosmo\prev, signed by key ... in /etc/pkg/keys
cosmo-upgrade: no boot environment pairs \cosmo\prev with a snapshot of the root ...
cosmo-upgrade: boot set switched back: ... boots the previous set; the one it replaced is in \cosmo\prev
cosmo-upgrade: rolled back; reboot to run it
```

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
