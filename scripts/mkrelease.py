#!/usr/bin/env python3
"""mkrelease.py ARCH DEST IMAGE LOADER KERNEL ARCHIVE MANIFEST CMDLINE

Collect one architecture's release set (roadmap M6; docs/development.md,
"Releases") into DEST, which is emptied first, under the names the GitHub
release carries:

    cosmoos-ARCH.img           the signed live image (GPT, ESP)
    cosmoos-ARCH-BOOTX64.EFI   the UEFI loader (BOOTAA64.EFI on AArch64)
    cosmoos-ARCH-kernel.elf    \\cosmo\\kernel.elf
    cosmoos-ARCH-boot.tar      \\cosmo\\boot.tar
    cosmoos-ARCH-cmdline       \\cosmo\\cmdline
    cosmoos-ARCH-manifest      \\cosmo\\manifest, signed over the three above
    SHA256SUMS                 `sha256sum -c` format, the files above

Every file is a copy of a build output; nothing here adds a timestamp,
so the set is as reproducible as the build (scripts/check-reproducible.sh).
"""
import hashlib
import os
import shutil
import sys


def main():
    if len(sys.argv) != 9:
        sys.exit(__doc__)
    arch, dest, image, loader, kernel, archive, manifest, cmdline = sys.argv[1:]
    shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(dest)
    files = [
        (image, f"cosmoos-{arch}.img"),
        (loader, f"cosmoos-{arch}-{os.path.basename(loader)}"),
        (kernel, f"cosmoos-{arch}-kernel.elf"),
        (archive, f"cosmoos-{arch}-boot.tar"),
        (cmdline, f"cosmoos-{arch}-cmdline"),
        (manifest, f"cosmoos-{arch}-manifest"),
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
