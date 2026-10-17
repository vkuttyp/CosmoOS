#!/usr/bin/env python3
"""check-release-version.py -- a release says its tag everywhere it names
a version (docs/development.md, "Releases").

    check-release-version.py --tag vX.Y.Z[-rcN] [--notes notes.md] SET_DIR...

For each release set (`make release`'s out/release/ARCH):

  - the kernel's build id -- the `(build ...)` of its banner and uname's
    version, COSMO_BUILD_ID from `git describe` -- is the tag: every
    describe-shaped string in cosmoos-ARCH-kernel.elf equals it (the live
    image carries the same file, bound to it by the signed manifest; its
    other contents name other versions, BusyBox's among them);
  - the files are the set's names (cosmoos-ARCH*, SHA256SUMS, its .sig;
    the names carry no version, so none can name another);
  - the cosmo-upgrade package's version is the one the tag gives
    (scripts/mkrelease.py package_version: v1.0.5 -> 1.0.5).

With --notes, the release notes name the version ("CosmoOS 1.0.5"), and
every "CosmoOS X.Y.Z" they contain is it.

v1.0.4 was built from a checkout whose annotated tag had become a
lightweight one (actions/checkout fetches a tag by its commit), so `git
describe` skipped it and the kernel said v1.0.3-123-g1ec37307 and the
package 1.0.3-123. Exit 0 when all agree, 1 naming each disagreement.
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import mkrelease  # noqa: E402
import pkgbuild  # noqa: E402

TAG = re.compile(r"^v\d+\.\d+\.\d+(-rc\d+)?$")
# What `git describe --always --dirty` can print for a tagged tree: the
# tag, or the tag plus a distance and hash, perhaps dirty.
DESCRIBE = re.compile(rb"(?<![\w.])v\d+\.\d+\.\d+(?:-rc\d+)?(?:-\d+-g[0-9a-f]{7,40})?(?:-dirty)?(?![\w.-])")
NOTES_VERSION = re.compile(r"CosmoOS (\d+\.\d+\.\d+(?:-rc\d+)?)\b")
SET_NAMES = ("{a}.img", "{a}-BOOTX64.EFI", "{a}-BOOTAA64.EFI", "{a}-kernel.elf", "{a}-boot.tar", "{a}-cmdline",
             "{a}-manifest", "{a}-cosmo-upgrade", "{a}-cosmo-upgrade.cpk")


def build_ids(path):
    with open(path, "rb") as f:
        return {m.group(0).decode() for m in DESCRIBE.finditer(f.read())}


def check_set(tag, d):
    bad = []
    names = sorted(os.listdir(d))
    arch = None
    for n in names:
        m = re.match(r"^cosmoos-(x86_64|aarch64)", n)
        if m:
            arch = m.group(1)
            break
    if arch is None:
        return [f"{d}: no cosmoos-ARCH file: not a release set"]
    allowed = {s.format(a=f"cosmoos-{arch}") for s in SET_NAMES} | {"SHA256SUMS", "SHA256SUMS.sig"}
    for n in names:
        if n not in allowed:
            bad.append(f"{d}: {n} is not a release set's file name")
    for f in (f"cosmoos-{arch}-kernel.elf",):
        p = os.path.join(d, f)
        if not os.path.exists(p):
            bad.append(f"{d}: {f} missing")
            continue
        ids = build_ids(p)
        if ids != {tag}:
            bad.append(f"{f}: build id {', '.join(sorted(ids)) or '(none)'}, not {tag}")
    cpk = os.path.join(d, f"cosmoos-{arch}-cosmo-upgrade.cpk")
    if os.path.exists(cpk):
        fields, _ = pkgbuild.read_manifest_from_package(cpk)
        want = mkrelease.package_version(tag)
        if fields.get("name") != "cosmo-upgrade" or fields.get("version") != want:
            bad.append(f"{os.path.basename(cpk)}: package {fields.get('name')}-{fields.get('version')}, "
                       f"not cosmo-upgrade-{want}")
    else:
        bad.append(f"{d}: {os.path.basename(cpk)} missing")
    return bad


def check_notes(tag, path):
    version = tag[1:]
    text = open(path, encoding="utf-8").read()
    seen = set(NOTES_VERSION.findall(text))
    if version not in seen:
        return [f"{path}: never says 'CosmoOS {version}'"]
    return [f"{path}: says 'CosmoOS {v}', not {version}" for v in sorted(seen - {version})]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--notes")
    ap.add_argument("sets", nargs="*")
    a = ap.parse_args()
    if not TAG.match(a.tag):
        ap.error(f"tag '{a.tag}' is not vX.Y.Z[-rcN]")
    if not a.sets and not a.notes:
        ap.error("nothing to check: give a release set or --notes")
    bad = []
    for d in a.sets:
        bad += check_set(a.tag, d)
    if a.notes:
        bad += check_notes(a.tag, a.notes)
    for b in bad:
        print(f"check-release-version: {b}", file=sys.stderr)
    if bad:
        return 1
    print(f"check-release-version: {a.tag} in {len(a.sets)} set(s){' and the notes' if a.notes else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
