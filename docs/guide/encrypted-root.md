# Encrypted root

The installer can encrypt the root: every file's contents and every
directory's names are ciphertext on the disk, and the pool's own records
-- which blocks are used, what each inode is, where things are -- are
authenticated, so a change made to them behind the system's back is
refused rather than believed. The machine asks for a passphrase at every
boot.

The steps below are the ones `make test-encrypt` runs (CI job `encrypt`,
on both architectures), and the output is from one of its x86-64 runs.
The guide's own runner does not type passphrases, so the steps are shown
here as text.

## Installing

From the live image, add `--encrypt`. The installer asks for the
passphrase twice, without echoing it, before it writes anything:

```text
cosmo$ cosmo-install --encrypt vdc
new passphrase:
new passphrase again:
cosmo-install: slot 0 sealed with the passphrase (argon2id 65536 KiB, 3 passes: 2.20 s)
cosmo-install: vdc: esp sectors 2048-133119, root sectors 133120-3145694
cosmo-install: copied 77 files in 14 directories
cosmo-install: installed on vdc: esp vdc1, root vdc2, partuuid 443d97fc-d143-4124-bf40-32dff008b4a4
cosmo-install: the root is encrypted (format 14): slot 0 the passphrase; it is asked for at every boot
```

If the two passphrases differ the installer says so and stops, and the
disk is as it was. With `--mirror DISK2` both disks hold one pool with
one key ([Mirrors](mirrors.md)).

The passphrase is not the key. The installer draws a random 256-bit
master key, which encrypts everything, and seals it with a key made from
the passphrase by Argon2id: 64 MiB of memory and three passes, which
take about a second (the line above says how long it took here). That
cost is paid once per guess, so a passphrase is only as strong as it is
long: use several words.

## Booting

At every boot, before the root is checked or mounted, the console asks:

```text
passphrase for cosmo-root:
cosmo-key: that passphrase opens no slot of cosmo-root
passphrase for cosmo-root:
cosmo-key: cosmo-root unlocked with slot 0 (argon2id 65536 KiB, 3 passes: 1.03 s)
cosmofs: root checked, 0 findings, 0 repaired
init: switched to the disk root ahci0p0p2 (root=boot-disk)
```

The prompt is on every console the machine has: the display and the
serial line. Nothing is echoed. After three wrong passphrases the
machine restarts with nothing mounted:

```text
cosmo-key: 3 wrong passphrases for cosmo-root
init: three wrong passphrases for the root; restarting
```

## More than one passphrase

A pool has eight key slots. Each can open it with a passphrase of its
own; all of them open the same master key, so adding or removing one
changes no file. `cosmo-key add` asks for a passphrase that already
works, then the new one twice:

```text
cosmo$ cosmo-key add
a passphrase for cosmo-root:
new passphrase:
new passphrase again:
cosmo-key: slot 1 of cosmo-root written (argon2id 65536 KiB, 3 passes: 1.04 s)
cosmo$ cosmo-key list
cosmo-root: format 14, encrypted, unlocked
slot 0: passphrase, argon2id 65536 KiB, 3 passes, 1 lane
slot 1: passphrase, argon2id 65536 KiB, 3 passes, 1 lane
slot 2: empty
```

- `cosmo-key change` gives the slot the current passphrase opens a new
  one. `--passes` and `--memory` raise Argon2id's cost for that slot.
- `cosmo-key remove SLOT` empties a slot, after a passphrase that opens
  the pool. The last slot cannot be removed.

Forgetting every passphrase loses the data: there is no way in without
one, and no copy of the master key anywhere else.

## A key file instead of a passphrase

`cosmo-install --encrypt --key-file` puts a random key file in slot 0,
written to the EFI System Partition (`\cosmo.key`), and the passphrase
in slot 1. The machine then boots without asking:

```text
cosmo-key: cosmo-root unlocked with the key file on the ESP
```

**The ESP is not encrypted.** Anyone holding the disk holds the key
file, so this protects nothing against a stolen disk; it is for a
machine that must restart unattended and whose disk does not leave the
building, or a disk whose key file you remove (`cosmo-key remove 0`) once
it is somewhere that matters. Without the file the console asks for the
passphrase in slot 1. It is off unless you ask for it.

## What it protects, and what it does not

- Encrypted (ChaCha20, each file under a key of its own): file contents
  and directory names.
- Authenticated, not encrypted (a 64-bit BLAKE2b tag under a key from
  the master key): every inode, map, bitmap, extent list, checksum
  block, snapshot record, member table and label, and both superblocks.
  Their shape is readable -- how many files, their sizes, owners, modes
  and times, the shape of the tree -- but a change to any of them is
  refused as unreadable, and a mirror repairs it from the other disk.
- Not protected: a block put back to an older version of itself, which
  verifies; and a disk replaced wholesale by another system, which boots
  as that system. The passphrase prompt is not covered by verified
  boot: it comes from the signed boot archive, but what you type is
  not measured.
- The key is held by the kernel while the root is mounted, never written
  out, and wiped when the pool is unmounted. It is never shown in
  `/proc` or `dmesg`.

Snapshots, rollback, boot environments, mirrors, resilvering, scrub and
`fsck.cosmofs` work on an encrypted root as on any other, once it is
unlocked.

## Upgrading and older releases

An encrypted root is format 14, which CosmoOS 1.0.12 and older cannot
read: `cosmo-upgrade` refuses to roll an encrypted root back to such a
release. A root installed without `--encrypt` keeps its format.
Encrypting an existing root in place is not possible: install to a new
disk with `--encrypt` and copy the data across.
