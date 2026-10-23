#!/usr/bin/env python3
"""Prove the three defects roadmap 1.2 B3's hotplug work found behind a
PCIe root port (docs/drivers/pci/design.md, "Hotplug").

    hotplug-iommu-probe.py --case NAME [--old] [--tree REV]

  vtd    VT-d took a DMAR sub-hierarchy scope (a bridge) to cover the     x86_64
         bridge alone, so a device behind a root port got no DMA domain;
         an NVMe controller hot-added there faults its first DMA
  smmu   the SMMUv3 stream table had 256 entries (bus 0), so a device     aarch64
         behind a root port (stream id 0x100 and up) got none either
  order  the hot-plug ports' interrupts were set up in pci_init, before   aarch64
         the IOMMU: the port has no domain, so behind the SMMU its MSI-X
         write faults and the attention button's event never arrives

--old restores the old behaviour in a throwaway worktree, by the
replacement the case names, and runs `make BUILD=release test-hotplug`
on the case's architecture: old mode passes when the test fails at the
case's step. Fixed mode passes when the test passes. Logs under
out/hotplug-iommu-probe/<case>-<mode>. Exit 0 when the mode shows what it
should, else 1.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

# arch, [(file, text, old text)], the test-hotplug FAIL line's pattern.
CASES = {
    "vtd": ("x86_64",
            [("drivers/iommu/intel_vtd.c", "        if (u->scope_type[i] != 2)\n            continue;",
              "        if (u->scope_type[i] != 2 || 1)\n            continue;")],
            r"test-hotplug: FAIL: NVMe add"),
    "smmu": ("aarch64",
             [("drivers/iommu/arm_smmuv3.c", "#define STRTAB_LOG2_MAX  12u", "#define STRTAB_LOG2_MAX  8u")],
             r"test-hotplug: FAIL: "),
    "order": ("aarch64",
              [("kernel/core/main.c",
                "    pci_hotplug_start();   /* the hot-plug ports' interrupts: an MSI is a DMA write, so after the IOMMU */\n",
                ""),
               ("drivers/pci/pci.c", "    kinfo(\"pci: %u device(s) enumerated\", g_count);\n    pciehp_init();\n",
                "    kinfo(\"pci: %u device(s) enumerated\", g_count);\n    pciehp_init();\n    pci_hotplug_start();\n")],
              r"test-hotplug: FAIL: QMP: no DEVICE_DELETED for hot0dev"),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--case", choices=sorted(CASES), required=True)
    ap.add_argument("--old", action="store_true")
    ap.add_argument("--tree", default="HEAD")
    a = ap.parse_args()
    arch, edits, fail_at = CASES[a.case]
    mode = "old" if a.old else "fixed"
    work = ROOT / "out" / "hotplug-iommu-probe" / f"{a.case}-{mode}"
    work.mkdir(parents=True, exist_ok=True)
    if any(work.iterdir()):
        sys.exit(f"refusing to overwrite nonempty probe output {work}")
    tree = Path(tempfile.mkdtemp(prefix=f"hip-{a.case}-", dir=ROOT / "out"))
    tree.rmdir()
    subprocess.run(["git", "-C", str(ROOT), "worktree", "add", "--detach", str(tree), a.tree], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        if a.old:
            for path, text, old in edits:
                src = tree / path
                body = src.read_text()
                if body.count(text) != 1:
                    sys.exit(f"{path}: the fixed text is not there exactly once; the probe is stale")
                src.write_text(body.replace(text, old))
        make = "gmake" if sys.platform == "darwin" else "make"
        args = [make, "-C", str(tree), "-j8", f"ARCH={arch}", "BUILD=release", f"OUT={tree / 'out' / 'rel'}",
                f"BB_DIR={ROOT / 'out' / f'busybox-{arch}'}", f"PORTS_CACHE={ROOT / '.cache' / 'ports'}"]
        with (work / "make.log").open("w") as log:
            rc = subprocess.run(args + ["test-hotplug"], stdin=subprocess.DEVNULL, stdout=log,
                                stderr=subprocess.STDOUT, timeout=2400).returncode
        text = (work / "make.log").read_text(errors="replace")
        if a.old:
            ok = rc != 0 and re.search(fail_at, text) is not None
            m = re.search(r"test-hotplug: FAIL: .*", text)
            seen = (f"`{m.group(0)}`" if m else f"no FAIL line (make exited {rc})")
        else:
            ok = rc == 0 and "test-hotplug: PASS" in text
            seen = "test-hotplug passed" if ok else f"make exited {rc}"
        print(f"PROBE: {'PASS' if ok else 'FAIL'} case={a.case} arch={arch} mode={mode}: {seen} ({work})")
        return 0 if ok else 1
    finally:
        subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)])


if __name__ == "__main__":
    sys.exit(main())
