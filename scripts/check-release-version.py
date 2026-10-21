#!/usr/bin/env python3
"""check-release-version.py -- a release says its tag everywhere it names
a version (docs/development.md, "Releases").

    check-release-version.py --tag vX.Y.Z[-rcN] [--notes notes.md] SET_DIR...
    check-release-version.py --kernel ELF --build-id ID

For each release set (`make release`'s out/release/ARCH):

  - the kernel's build id -- the `(build ...)` of its banner and uname's
    version, COSMO_BUILD_ID from `git describe` -- is the tag: every
    describe-shaped string in cosmoos-ARCH-kernel.elf equals it (the live
    image carries the same file, bound to it by the signed manifest; its
    other contents name other versions, BusyBox's among them);
  - the files are exactly the set's (scripts/mkrelease.py): every one of
    them present, the loader the architecture's own (BOOTX64.EFI,
    BOOTAA64.EFI), and nothing else (the names carry no version, so none
    can name another);
  - the cosmo-upgrade package's version is the one the tag gives
    (scripts/mkrelease.py package_version: v1.0.5 -> 1.0.5).

With --notes, the release notes name the version ("CosmoOS 1.0.5"), and
every "CosmoOS X.Y.Z" they contain is it.

v1.0.4 was built from a checkout whose annotated tag had become a
lightweight one (actions/checkout fetches a tag by its commit), so `git
describe` skipped it and the kernel said v1.0.3-123-g1ec37307 and the
package 1.0.3-123. Exit 0 when all agree, 1 naming each disagreement.

With --kernel, the one rule a build can break before it is tagged: the
kernel holds no describe-shaped string but its own build id (ID, `git
describe --always --dirty`; a bare hash when no tag is reachable, and then
none), and holds ID when it is describe-shaped. CI checks every release build so; v1.0.8's first tag failed the
release on a kernel message that said "v1.0.7".
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
LOADER = {"x86_64": "BOOTX64.EFI", "aarch64": "BOOTAA64.EFI"}
SET_NAMES = ("{a}.img", "{a}-{loader}", "{a}-kernel.elf", "{a}-boot.tar", "{a}-cmdline", "{a}-manifest",
             "{a}-cosmo-upgrade", "{a}-cosmo-upgrade.cpk", "SHA256SUMS", "SHA256SUMS.sig")


def build_ids(path):
    with open(path, "rb") as f:
        return {m.group(0).decode() for m in DESCRIBE.finditer(f.read())}


def check_set(tag, d):
    bad = []
    names = sorted(os.listdir(d))
    arches = {m.group(1) for m in (re.match(r"^cosmoos-(x86_64|aarch64)", n) for n in names) if m}
    if len(arches) != 1:
        return [f"{d}: files of {len(arches)} architectures ({', '.join(sorted(arches)) or 'none'}): "
                "not one release set"]
    arch = arches.pop()
    want = {s.format(a=f"cosmoos-{arch}", loader=LOADER[arch]) for s in SET_NAMES}
    for n in names:
        if n not in want:
            bad.append(f"{d}: {n} is not a file of {arch}'s release set")
    for n in sorted(want - set(names)):
        bad.append(f"{d}: {n} missing")
    for f in (f"cosmoos-{arch}-kernel.elf",):
        p = os.path.join(d, f)
        if not os.path.exists(p):
            continue   # said above
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
    ap.add_argument("--tag")
    ap.add_argument("--notes")
    ap.add_argument("--kernel")
    ap.add_argument("--build-id")
    ap.add_argument("sets", nargs="*")
    a = ap.parse_args()
    if a.kernel:
        if a.build_id is None or a.tag or a.notes or a.sets:
            ap.error("--kernel takes --build-id and nothing else")
        ids = build_ids(a.kernel)
        other = sorted(ids - {a.build_id})
        if other:
            print(f"check-release-version: {a.kernel}: describe-shaped {', '.join(other)}, "
                  f"not its build id {a.build_id}", file=sys.stderr)
            return 1
        # A tagged build id must be there too, as the release check requires.
        if DESCRIBE.fullmatch(a.build_id.encode()) and a.build_id not in ids:
            print(f"check-release-version: {a.kernel}: never says its build id {a.build_id}", file=sys.stderr)
            return 1
        print(f"check-release-version: {a.kernel}: no describe-shaped string but {a.build_id}")
        return 0
    if not a.tag:
        ap.error("--tag is required")
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
