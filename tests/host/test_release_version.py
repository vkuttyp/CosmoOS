#!/usr/bin/env python3
"""Host test of scripts/check-release-version.py and scripts/release-tag.sh
(make host-test; docs/development.md, "Releases").

check-release-version.py, on synthetic release sets:
- a set whose kernel says the tag and whose package has the tag's version
  passes, with notes that say "CosmoOS <version>";
- a kernel with the previous tag's describe (v1.0.4's real defect,
  v1.0.3-123-g1ec37307) fails and the message names it;
- a package version 1.0.3-123 fails;
- a file name no set has fails, and so do a missing file and the other
  architecture's loader;
- notes that also say another "CosmoOS X.Y.Z", or never say this one, fail.

check-release-version.py --kernel, on a synthetic kernel:
- its own build id alone passes, a dev build's (v1.0.7-24-g5c7b507f-dirty)
  as well as a tag;
- a message saying "(v1.0.7 and older)" beside the build id fails, named
  (v1.0.8's first tag), and "(CosmoOS 1.0.7 and older)" passes;
- a bare-hash build id (no tag reachable) with no describe-shaped string
  passes;
- --kernel with --tag is refused.

release-tag.sh, in a clone of a local repository:
- a lightweight tag, as actions/checkout leaves an annotated one, is
  replaced by the annotated tag from origin, and git describe then names it;
- a tag that is lightweight on origin too is refused;
- a malformed tag is refused before git sees it.
"""
import os
import secrets
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHECK = os.path.join(ROOT, "scripts", "check-release-version.py")
TAG_SH = os.path.join(ROOT, "scripts", "release-tag.sh")
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import pkgbuild  # noqa: E402


def check(cond, what):
    if not cond:
        print(f"FAIL: {what}")
        sys.exit(1)
    print(f"ok: {what}")


def make_set(d, key, build_id, version, extra=None):
    os.makedirs(d)
    with open(os.path.join(d, "cosmoos-aarch64-kernel.elf"), "wb") as f:
        f.write(b"\x7fELF\0" + b"CosmoOS\0%s kernel %s (build %s)\n\0" + build_id.encode() + b"\0debug\0")
    with open(os.path.join(d, "cosmoos-aarch64-cosmo-upgrade.cpk"), "wb") as f:
        f.write(pkgbuild.pack("cosmo-upgrade", version, "test", [("sbin/cosmo-upgrade", 0o755, b"x")], key))
    for n in ("cosmoos-aarch64.img", "cosmoos-aarch64-BOOTAA64.EFI", "cosmoos-aarch64-boot.tar",
              "cosmoos-aarch64-cmdline", "cosmoos-aarch64-manifest", "cosmoos-aarch64-cosmo-upgrade",
              "SHA256SUMS", "SHA256SUMS.sig") + ((extra,) if extra else ()):
        open(os.path.join(d, n), "wb").close()
    return d


def run(*args):
    return subprocess.run([sys.executable, CHECK, *args], capture_output=True, text=True)


def git(cwd, *args, check_rc=True):
    env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t",
               GIT_COMMITTER_EMAIL="t@t", GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_NOSYSTEM="1")
    r = subprocess.run(["git", "-C", cwd, *args], capture_output=True, text=True, env=env)
    if check_rc and r.returncode != 0:
        print(r.stdout + r.stderr)
        sys.exit(f"FAIL: git {' '.join(args)}")
    return r


def tag_sh(cwd, tag):
    env = dict(os.environ, GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_NOSYSTEM="1")
    return subprocess.run(["sh", TAG_SH, tag], cwd=cwd, capture_output=True, text=True, env=env)


def main():
    with tempfile.TemporaryDirectory() as t:
        key = os.path.join(t, "pkg.key")
        with open(key, "w") as f:
            f.write(secrets.token_hex(32) + "\n")

        good = make_set(os.path.join(t, "good"), key, "v1.0.5", "1.0.5")
        notes = os.path.join(t, "notes.md")
        with open(notes, "w") as f:
            f.write("## Boots with any RAM size\n\nCosmoOS 1.0.5: ... v1.0.0-v1.0.4 hung ...\n")
        r = run("--tag", "v1.0.5", "--notes", notes, good)
        check(r.returncode == 0, "a set and notes that say v1.0.5 pass")

        stale = make_set(os.path.join(t, "stale"), key, "v1.0.3-123-g1ec37307", "1.0.5")
        r = run("--tag", "v1.0.5", stale)
        check(r.returncode == 1 and "v1.0.3-123-g1ec37307" in r.stderr, "the previous tag's describe fails, named")

        pkg = make_set(os.path.join(t, "pkg"), key, "v1.0.5", "1.0.3-123")
        r = run("--tag", "v1.0.5", pkg)
        check(r.returncode == 1 and "cosmo-upgrade-1.0.3-123" in r.stderr, "a package version 1.0.3-123 fails")

        extra = make_set(os.path.join(t, "extra"), key, "v1.0.5", "1.0.5", extra="cosmoos-1.0.4-aarch64.img")
        r = run("--tag", "v1.0.5", extra)
        check(r.returncode == 1 and "cosmoos-1.0.4-aarch64.img" in r.stderr, "a file name no set has fails")

        missing = make_set(os.path.join(t, "missing"), key, "v1.0.5", "1.0.5")
        os.remove(os.path.join(missing, "cosmoos-aarch64-boot.tar"))
        r = run("--tag", "v1.0.5", missing)
        check(r.returncode == 1 and "cosmoos-aarch64-boot.tar missing" in r.stderr, "a missing file fails, named")

        wrong = make_set(os.path.join(t, "wrong"), key, "v1.0.5", "1.0.5")
        os.rename(os.path.join(wrong, "cosmoos-aarch64-BOOTAA64.EFI"), os.path.join(wrong, "cosmoos-aarch64-BOOTX64.EFI"))
        r = run("--tag", "v1.0.5", wrong)
        check(r.returncode == 1 and "BOOTX64.EFI is not a file of aarch64's" in r.stderr
              and "BOOTAA64.EFI missing" in r.stderr, "the other architecture's loader fails")

        with open(notes, "w") as f:
            f.write("CosmoOS 1.0.5: ...\n\nAs in CosmoOS 1.0.4, ...\n")
        r = run("--tag", "v1.0.5", "--notes", notes)
        check(r.returncode == 1 and "CosmoOS 1.0.4" in r.stderr, "notes naming another version fail")
        with open(notes, "w") as f:
            f.write("CosmoOS: ...\n")
        r = run("--tag", "v1.0.5", "--notes", notes)
        check(r.returncode == 1, "notes that never name the version fail")

        r = run("--tag", "v1.0.5;rm", good)
        check(r.returncode != 0, "a malformed tag is refused")

        def kernel(name, *strings):
            p = os.path.join(t, name)
            with open(p, "wb") as f:
                f.write(b"\x7fELF\0" + b"\0".join(s.encode() for s in strings) + b"\0")
            return p
        dev = "v1.0.7-24-g5c7b507f-dirty"
        r = run("--kernel", kernel("k-dev", dev, "debug"), "--build-id", dev)
        check(r.returncode == 0, "a kernel holding its own dev build id alone passes")
        r = run("--kernel", kernel("k-tag", "v1.0.8"), "--build-id", "v1.0.8")
        check(r.returncode == 0, "a kernel holding its tag alone passes")
        msg = "a kernel that reads format 11 at most ({} and older) cannot mount this pool"
        r = run("--kernel", kernel("k-old", "v1.0.8", msg.format("v1.0.7")), "--build-id", "v1.0.8")
        check(r.returncode == 1 and "describe-shaped v1.0.7, not its build id v1.0.8" in r.stderr,
              "a message naming v1.0.7 beside the build id fails, named")
        r = run("--kernel", kernel("k-new", "v1.0.8", msg.format("CosmoOS 1.0.7")), "--build-id", "v1.0.8")
        check(r.returncode == 0, "the message saying CosmoOS 1.0.7 passes")
        r = run("--kernel", kernel("k-hash", "5c7b507f", "debug"), "--build-id", "5c7b507f")
        check(r.returncode == 0, "a bare-hash build id with no describe-shaped string passes")
        r = run("--kernel", kernel("k-tag2", "v1.0.8"), "--build-id", "v1.0.8", "--tag", "v1.0.8")
        check(r.returncode != 0, "--kernel with --tag is refused")

        # release-tag.sh against a local origin.
        origin = os.path.join(t, "origin")
        os.makedirs(origin)
        git(origin, "init", "-q")
        with open(os.path.join(origin, "f"), "w") as f:
            f.write("a\n")
        git(origin, "add", "f")
        git(origin, "commit", "-qm", "one")
        git(origin, "tag", "-a", "v1.0.4", "-m", "v1.0.4")
        git(origin, "commit", "-q", "--allow-empty", "-m", "two")
        git(origin, "tag", "-a", "v1.0.5", "-m", "v1.0.5")
        git(origin, "tag", "v1.0.6")   # lightweight on origin too
        clone = os.path.join(t, "clone")
        subprocess.run(["git", "clone", "-q", "--no-tags", origin, clone], check=True, capture_output=True)
        git(clone, "fetch", "-q", "origin", "refs/tags/v1.0.4:refs/tags/v1.0.4")
        # What actions/checkout leaves: the pushed tag as the bare commit.
        git(clone, "tag", "v1.0.5", "HEAD")
        check(git(clone, "describe", "--always", "--dirty").stdout.strip().startswith("v1.0.4-1-g"),
              "the checkout's lightweight tag: git describe names the previous tag")
        r = tag_sh(clone, "v1.0.5")
        check(r.returncode == 0 and git(clone, "describe", "--always", "--dirty").stdout.strip() == "v1.0.5",
              "release-tag.sh restores the annotated tag; git describe names it")
        git(clone, "tag", "v1.0.6", "HEAD")
        r = tag_sh(clone, "v1.0.6")
        check(r.returncode == 1 and "lightweight" in r.stderr, "a tag lightweight on origin is refused")
        r = tag_sh(clone, "v1.0.6; true")
        check(r.returncode == 1 and "is not vX.Y.Z" in r.stderr, "a malformed tag is refused by release-tag.sh")
    return 0


if __name__ == "__main__":
    sys.exit(main())
