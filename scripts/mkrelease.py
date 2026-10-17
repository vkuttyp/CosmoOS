#!/usr/bin/env python3
"""mkrelease.py ARCH DEST IMAGE LOADER KERNEL ARCHIVE MANIFEST CMDLINE UPGRADE VERIFY KEY PKGKEY BUILD_ID

Collect one architecture's release set (roadmap M6; docs/development.md,
"Releases") into DEST under the names the GitHub release carries. DEST is
created if missing; a DEST that holds anything but an earlier release set
(cosmoos-* and SHA256SUMS*) is refused rather than emptied, so a mistyped
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
    cosmoos-ARCH-cosmo-upgrade.cpk
                               a package of cosmo-upgrade and cosmo-verify,
                               signed with PKGKEY, which a v1.0.3 system's
                               `pkg install -n` checks against the key it
                               shipped with (docs/guide/upgrading.md)
    SHA256SUMS                 `sha256sum -c` format, the files above
    SHA256SUMS.sig             its detached signature with KEY: the
                               COSMOSIG trailer of scripts/modsign.py on its
                               own. The published SHA256SUMS joins both
                               architectures' and is signed again whole by
                               the release workflow.

Every file is a copy of a build output or a pure function of them, the
build id (the package's version) and the keys (Ed25519 is deterministic);
nothing here adds a timestamp, so the set is as reproducible as the build
(scripts/check-reproducible.sh).
"""
import hashlib
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import modsign  # noqa: E402
import pkgbuild  # noqa: E402


def package_version(build_id):
    """v1.0.4 -> 1.0.4; v1.0.3-12-gabc -> 1.0.3-12; anything else 0."""
    m = re.match(r"^v(\d+(?:\.\d+)*)(?:-(\d+)-g[0-9a-f]+)?(?:-dirty)?$", build_id)
    if not m:
        return "0"
    return m.group(1) + (f"-{m.group(2)}" if m.group(2) else "")


def main():
    if len(sys.argv) != 14:
        sys.exit(__doc__)
    arch, dest, image, loader, kernel, archive, manifest, cmdline, upgrade, verify, key, pkgkey, build_id = sys.argv[1:]
    os.makedirs(dest, exist_ok=True)
    stray = [f for f in os.listdir(dest) if not (f.startswith("cosmoos-") or f in ("SHA256SUMS", "SHA256SUMS.sig"))]
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
    tools = []
    for src, name in ((upgrade, "cosmo-upgrade"), (verify, "cosmo-verify")):
        with open(src, "rb") as f:
            tools.append((f"sbin/{name}", 0o755, f.read()))
    cpk = os.path.join(dest, f"cosmoos-{arch}-cosmo-upgrade.cpk")
    with open(cpk, "wb") as f:
        f.write(pkgbuild.pack("cosmo-upgrade", package_version(build_id),
                              "cosmo-upgrade and cosmo-verify of this release", tools, pkgkey))
    sums = []
    for src, name in files:
        shutil.copyfile(src, os.path.join(dest, name))
    for _, name in files + [(cpk, os.path.basename(cpk))]:
        with open(os.path.join(dest, name), "rb") as f:
            sums.append(f"{hashlib.sha256(f.read()).hexdigest()}  {name}\n")
    text = "".join(sums).encode()
    with open(os.path.join(dest, "SHA256SUMS"), "wb") as f:
        f.write(text)
    with open(os.path.join(dest, "SHA256SUMS.sig"), "wb") as f:
        f.write(modsign.detached(modsign.read_hex_key(key, "secret key"), text))
    sys.stdout.writelines(sums)


if __name__ == "__main__":
    main()
