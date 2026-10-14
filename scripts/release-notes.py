#!/usr/bin/env python3
"""release-notes.py -- the release notes for a tagged release (roadmap M6;
docs/development.md, "Releases").

    release-notes.py --tag v1.0.0 --toolchain REF --litmus REF
                     --packages packages.txt --pub tools/keys/<release>.pub
                     --sums SHA256SUMS --repro "x86_64: ..." --repro "aarch64: ..."
                     [--out notes.md]

Writes Markdown: what the release is, the release signing key and its
fingerprint (key id: the first 8 bytes of SHA-512 of the public key, as
scripts/modsign.py and the loader print it; the full key is the
fingerprint), the CI image digests the release was built in and the
compiler and QEMU versions inside them, BusyBox, musl, compiler-rt and
dropbear with the SHA-256 their sources are pinned by, the reproducibility
result, every file's SHA-256, and the known limitations from
docs/guide/limitations.md.
"""
import argparse
import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def mk_vars(path):
    """NAME := value lines of a make fragment."""
    out = {}
    for line in open(os.path.join(ROOT, path), encoding="utf-8"):
        m = re.match(r"^([A-Z0-9_]+)\s*:=\s*(\S+)\s*$", line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--toolchain", required=True)
    ap.add_argument("--litmus", required=True)
    ap.add_argument("--packages", required=True)
    ap.add_argument("--pub", required=True)
    ap.add_argument("--sums", required=True)
    ap.add_argument("--repro", action="append", default=[])
    ap.add_argument("--out")
    a = ap.parse_args()

    pub_hex = open(a.pub).read().strip()
    pub = bytes.fromhex(pub_hex)
    if len(pub) != 32:
        sys.exit(f"release-notes: {a.pub} is not a 32-byte Ed25519 public key")
    key_id = hashlib.sha512(pub).digest()[:8].hex()
    pkgs = {}
    for line in open(a.packages):
        parts = line.split()
        if len(parts) == 2:
            pkgs[parts[0]] = parts[1]
    bb = mk_vars("ports/busybox/busybox.mk")
    db = mk_vars("ports/dropbear/dropbear.mk")
    ossh = mk_vars("ports/openssh/openssh.mk")
    tls = mk_vars("ports/mbedtls/mbedtls.mk")
    lim = open(os.path.join(ROOT, "docs", "guide", "limitations.md"), encoding="utf-8").read()
    lim = lim.split("\n", 1)[1].strip()   # drop the page title
    lim = lim.replace("](../", "](https://github.com/vkuttyp/CosmoOS/blob/" + a.tag + "/docs/")
    lim = re.sub(r"\]\(([a-z-]+\.md)\)", r"](https://github.com/vkuttyp/CosmoOS/blob/" + a.tag + r"/docs/guide/\1)", lim)
    version = a.tag.lstrip("v")
    n = []
    n.append(f"CosmoOS {version}: the [1.0 roadmap](https://github.com/vkuttyp/CosmoOS/blob/{a.tag}/docs/roadmap-1.0.md) "
             "completed. CosmoOS installs to a disk, reboots into it, is used through BusyBox `ash`, keeps files "
             "across reboots, reaches the network by name, accepts an ssh login, and has a verified boot chain and "
             "sound randomness, on x86-64 and AArch64 under QEMU.\n")
    n.append(f"Start with the [user guide](https://github.com/vkuttyp/CosmoOS/blob/{a.tag}/docs/guide/README.md). "
             "Each architecture's files: `cosmoos-ARCH.img` (the signed live image), its UEFI loader, `kernel.elf`, "
             "`boot.tar`, `cmdline` and the signed `manifest`. Check them with `sha256sum --ignore-missing -c SHA256SUMS`.\n")
    n.append("## Release signing key\n")
    n.append("The loader in these images verifies the kernel, boot archive and command line against this Ed25519 "
             "key (and the kernel trusts it for modules and packages):\n")
    n.append(f"- public key: `{pub_hex}`")
    n.append(f"- key id: `{key_id}` (the first 8 bytes of SHA-512 of the key; the boot log prints it: "
             f"`verified boot, key {key_id}`)")
    n.append(f"- in the repository: `{os.path.relpath(a.pub, ROOT)}`\n")
    n.append("## Toolchain\n")
    n.append("Built and tested in the project's CI images (`ci/Dockerfile`), pinned by digest:\n")
    n.append(f"- toolchain: `{a.toolchain}`")
    n.append(f"- litmus: `{a.litmus}`")
    names = ["clang", "lld", "llvm", "qemu-system-x86", "qemu-system-arm", "ovmf", "qemu-efi-aarch64", "mtools", "python3"]
    # The versioned packages the metapackages depend on: what actually ran.
    names += sorted(p for p in pkgs if re.match(r"^(clang|lld|llvm)-\d+$", p))
    for p in names:
        if p in pkgs:
            n.append(f"- {p} {pkgs[p]}")
    n.append("")
    n.append("## Third-party sources\n")
    n.append("Fetched and checked against these SHA-256 digests (`ports/busybox/busybox.mk`, "
             "`ports/dropbear/dropbear.mk`, `ports/openssh/openssh.mk`, `ports/mbedtls/mbedtls.mk`):\n")
    n.append("| Source | Version | SHA-256 |\n|---|---|---|")
    n.append(f"| BusyBox | {bb['BUSYBOX_VERSION']} | `{bb['BUSYBOX_SHA256']}` |")
    n.append(f"| musl | {bb['MUSL_VERSION']} | `{bb['MUSL_SHA256']}` |")
    n.append(f"| compiler-rt | {bb['CRT_VERSION']} | `{bb['CRT_SHA256']}` |")
    n.append(f"| dropbear | {db['DROPBEAR_VERSION']} | `{db['DROPBEAR_SHA256']}` |")
    n.append(f"| OpenSSH portable (sftp-server only) | {ossh['OPENSSH_VERSION']} | `{ossh['OPENSSH_SHA256']}` |")
    n.append(f"| mbedTLS | {tls['MBEDTLS_VERSION']} | `{tls['MBEDTLS_SHA256']}` |")
    n.append(f"| CA certificates (curl's extract of Mozilla's) | {tls['CACERT_DATE']} | `{tls['CACERT_SHA256']}` |\n")
    n.append("## Reproducibility\n")
    n.append(f"Each architecture was built twice from the tag `{a.tag}`, in two fresh containers of the toolchain "
             "image above, with the release key; every file below was identical in both:\n")
    for r in a.repro:
        n.append(f"- {r}")
    n.append("\nTo rebuild: check out the tag, run the toolchain image above, and `make ARCH=<arch> "
             f"SIGNING=release MODSIGN_KEY=<key> KEYRING_PUBS={os.path.relpath(a.pub, ROOT)} release`. Without the "
             "private key, a build with your own key differs in what the key reaches: the key ring compiled into "
             "the kernel and loader, the signatures on modules, packages and the manifest, and the archive and "
             "image that hold them.\n")
    n.append("## SHA256SUMS\n")
    n.append("```")
    n.append(open(a.sums).read().rstrip())
    n.append("```\n")
    n.append("## Known limitations\n")
    n.append(lim)
    text = "\n".join(n) + "\n"
    if a.out:
        open(a.out, "w").write(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
