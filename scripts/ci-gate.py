#!/usr/bin/env python3
"""
ci-gate.py - what a CI run runs (docs/build/testing.md, "Which jobs a pull
request runs").

CI's `gate` job runs this first and every other job follows its outputs:

  run=false    a pull request was labelled with a label other than full-ci:
               nothing changed, so nothing runs (the run the push started
               stands).
  heavy=true   the two long acceptance tests run: the large-file test and
               the encrypted root's `encrypt` part (each about an hour or
               40 minutes of QEMU under TCG). They always run on a push to
               main and when run by hand; on a pull request, when it
               carries the full-ci label or changes a path they exercise
               (HEAVY_PATHS).

Everything else runs on every pull request. A regression the gate lets
through on a pull request is caught by the push to main that merges it.

Inputs, from the workflow's environment: EVENT (github.event_name), ACTION
(github.event.action), LABEL (the label just added, for `labeled`), LABELS
(the pull request's labels, a JSON list of names), BASE and HEAD (the pull
request's base and head commits). Outputs go to $GITHUB_OUTPUT when set,
and a line saying why to stdout.

    python3 scripts/ci-gate.py            # in CI
    EVENT=pull_request BASE=origin/main HEAD=HEAD LABELS='[]' python3 scripts/ci-gate.py   # locally
"""

import json
import os
import subprocess
import sys

FULL_CI = "full-ci"

# A changed path under one of these (or equal to one) runs the long tests on a pull request: the file system, the
# block and storage layers and their drivers, memory (the page cache and its dirty bound), booting and boot
# environments, installing, upgrading and keys, the harness and the tests themselves, and the build and CI.
HEAVY_PATHS = [
    "kernel-services/filesystem/cosmofs/",
    "kernel-services/vfs/",
    "kernel-services/storage/",
    "kernel/block/",
    "kernel/memory/",
    "kernel/include/kernel/cosmofs.h",
    "kernel/include/kernel/vfs.h",
    "kernel/include/uapi/cosmo/blkctl.h",
    "kernel/include/uapi/cosmo/fsctl.h",
    "kernel/include/uapi/cosmo/keyslot.h",
    "drivers/storage/",
    "drivers/nvme/",
    "drivers/virtio/",
    "boot/",
    "userland/system/",
    "userland/lib/fskey.",
    "userland/lib/keyslot.",
    "pkg/",
    "tests/boot/",
    "tools/fetch-release.py",
    "tools/keys/",
    "Makefile",
    "build/",
    "ci/",
    "scripts/",
    ".github/workflows/ci.yml",
]


def heavy_paths(paths: list[str]) -> list[str]:
    return [p for p in paths if any(p == h or (h.endswith(("/", ".")) and p.startswith(h)) for h in HEAVY_PATHS)]


def decide(env: dict[str, str], changed) -> tuple[bool, bool, str]:
    """(run, heavy, why); `changed` is called for the changed paths only when they matter."""
    event, action = env.get("EVENT", ""), env.get("ACTION", "")
    if event != "pull_request":
        return True, True, f"{event or 'a run'}: everything runs"
    if action == "labeled" and env.get("LABEL", "") != FULL_CI:
        return False, False, f"labelled '{env.get('LABEL', '')}': nothing to run"
    if FULL_CI in json.loads(env.get("LABELS") or "[]"):
        return True, True, f"the pull request is labelled {FULL_CI}: everything runs"
    hits = heavy_paths(changed())
    if hits:
        more = f" and {len(hits) - 3} more" if len(hits) > 3 else ""
        return True, True, f"the long tests run: {', '.join(hits[:3])}{more} changed"
    return True, False, (f"the long tests are skipped: no path they exercise changed "
                         f"(label the pull request {FULL_CI} to run them)")


def changed_paths(base: str, head: str) -> list[str]:
    out = subprocess.run(["git", "diff", "--name-only", f"{base}...{head}"], check=True, capture_output=True,
                         text=True).stdout
    return [line for line in out.splitlines() if line]


def main() -> int:
    env = dict(os.environ)
    run, heavy, why = decide(env, lambda: changed_paths(env["BASE"], env["HEAD"]))
    print(f"ci-gate: {why}")
    target = env.get("GITHUB_OUTPUT")
    if target:
        with open(target, "a", encoding="utf-8") as f:
            f.write(f"run={'true' if run else 'false'}\nheavy={'true' if heavy else 'false'}\n")
    else:
        print(f"run={'true' if run else 'false'} heavy={'true' if heavy else 'false'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
