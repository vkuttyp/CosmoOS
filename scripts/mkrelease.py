#!/usr/bin/env python3
"""mkrelease.py ARCH DEST IMAGE LOADER KERNEL ARCHIVE MANIFEST CMDLINE UPGRADE

Collect one architecture's release set (roadmap M6; docs/development.md,
"Releases") into DEST under the names the GitHub release carries. DEST is
created if missing; a DEST that holds anything but an earlier release set
(cosmoos-* and SHA256SUMS) is refused rather than emptied, so a mistyped
RELEASE_DIR deletes nothing:

    cosmoos-ARCH.img           the signed live image (GPT, ESP)
    cosmoos-ARCH-BOOTX64.EFI   the UEFI loader (BOOTAA64.EFI on AArch64)
    cosmoos-ARCH-kernel.elf    \\cosmo\\kernel.elf
    cosmoos-ARCH-boot.tar      \\cosmo\\boot.tar
    cosmoos-ARCH-cmdline       \\cosmo\\cmdline
    cosmoos-ARCH-manifest      \\cosmo\\manifest, signed over the three above
    cosmoos-ARCH-cosmo-upgrade the release's /sbin/cosmo-upgrade, which an
                               installed system runs to move to this release
                               (shell completeness S7)
    SHA256SUMS                 `sha256sum -c` format, the files above

Every file is a copy of a build output; nothing here adds a timestamp,
so the set is as reproducible as the build (scripts/check-reproducible.sh).
"""
import hashlib
import os
import shutil
import sys


def main():
    if len(sys.argv) != 10:
        sys.exit(__doc__)
    arch, dest, image, loader, kernel, archive, manifest, cmdline, upgrade = sys.argv[1:]
    os.makedirs(dest, exist_ok=True)
    stray = [f for f in os.listdir(dest) if not (f.startswith("cosmoos-") or f == "SHA256SUMS")]
    if stray:
        sys.exit(f"mkrelease: {dest} holds {', '.join(sorted(stray)[:5])}, which no release set has; "
                 "refusing to write the set over it")
    for f in os.listdir(dest):
        os.remove(os.path.join(dest, f))   # the earlier set: no name of it may outlive this one
    files = [
        (image, f"cosmoos-{arch}.img"),
        (loader, f"cosmoos-{arch}-{os.path.basename(loader)}"),
        (kernel, f"cosmoos-{arch}-kernel.elf"),
        (archive, f"cosmoos-{arch}-boot.tar"),
        (cmdline, f"cosmoos-{arch}-cmdline"),
        (manifest, f"cosmoos-{arch}-manifest"),
        (upgrade, f"cosmoos-{arch}-cosmo-upgrade"),
    ]
    sums = []
    for src, name in files:
        shutil.copyfile(src, os.path.join(dest, name))
        with open(src, "rb") as f:
            sums.append(f"{hashlib.sha256(f.read()).hexdigest()}  {name}\n")
    with open(os.path.join(dest, "SHA256SUMS"), "w") as f:
        f.writelines(sums)
    sys.stdout.writelines(sums)


if __name__ == "__main__":
    main()
