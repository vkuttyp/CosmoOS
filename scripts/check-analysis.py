#!/usr/bin/env python3
"""Compare Clang plist reports with the reviewed target baseline."""

import argparse
from collections import Counter
import json
from pathlib import Path
import plistlib
import sys


KEY_FIELDS = ("translation_unit", "checker", "file", "function", "message")


def key(entry):
    values = tuple(entry[name] for name in KEY_FIELDS)
    if any(not isinstance(value, str) or not value for value in values):
        raise ValueError("diagnostic key fields must be nonempty strings")
    return values


def relative(path, root):
    return Path(path).resolve().relative_to(root).as_posix()


def read_baseline(path, arch, compiler):
    baseline = json.loads(path.read_text())
    if baseline["schema"] != 2 or baseline["architecture"] != arch:
        raise ValueError("baseline schema or architecture mismatch")
    variants = baseline["compilers"]
    if not isinstance(variants, list) or not variants:
        raise ValueError("baseline compiler variants must be a nonempty list")
    selected = None
    versions = set()
    for variant in variants:
        version = variant["version"]
        if not isinstance(version, str) or not version or version in versions:
            raise ValueError("baseline compiler versions must be unique nonempty strings")
        versions.add(version)
        counts = Counter()
        for entry in variant["diagnostics"]:
            identity = key(entry)
            if identity in counts:
                raise ValueError("duplicate baseline key")
            if not isinstance(entry["reason"], str) or not entry["reason"].strip():
                raise ValueError("every baseline entry needs a reason")
            count = entry["count"]
            if type(count) is not int or count < 1:
                raise ValueError("baseline count must be a positive integer")
            counts[identity] = count
        if version == compiler:
            selected = counts
    if selected is None:
        raise ValueError("no reviewed baseline for compiler: " + compiler)
    return selected


def read_reports(paths, root, out):
    diagnostics = []
    compilers = set()
    seen = set()
    for path in paths:
        tu = path.resolve().relative_to(out).with_suffix(".c").as_posix()
        if tu in seen:
            raise ValueError("duplicate report: " + tu)
        seen.add(tu)
        report = plistlib.loads(path.read_bytes())
        files = report["files"]
        findings = report["diagnostics"]
        if not isinstance(files, list) or not isinstance(findings, list):
            raise ValueError("invalid report arrays: " + str(path))
        if not isinstance(report["clang_version"], str):
            raise ValueError("missing compiler identity: " + str(path))
        compilers.add(report["clang_version"])
        for finding in findings:
            loc = finding["location"]
            index = loc["file"]
            if type(index) is not int or not 0 <= index < len(files):
                raise ValueError("invalid diagnostic file index: " + str(path))
            entry = {
                "translation_unit": tu,
                "checker": finding["check_name"],
                "file": relative(files[index], root),
                "function": finding.get("issue_context", "<global>"),
                "message": finding["description"],
                "line": loc["line"],
                "column": loc["col"],
            }
            key(entry)
            diagnostics.append(entry)
    return diagnostics, sorted(compilers)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--arch", choices=("x86_64", "aarch64"), required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("reports", type=Path, nargs="+")
    args = parser.parse_args()
    try:
        findings, compilers = read_reports(args.reports, args.root.resolve(), args.out.resolve())
        if len(compilers) != 1:
            raise ValueError("reports must have exactly one compiler identity")
        args.inventory.write_text(json.dumps({
            "architecture": args.arch,
            "compilers": compilers,
            "diagnostics": sorted(findings, key=key),
        }, indent=2) + "\n")
        allowed = read_baseline(args.baseline, args.arch, compilers[0])
    except (OSError, ValueError, KeyError, TypeError, IndexError, plistlib.InvalidFileException) as error:
        print("static analysis: invalid input: " + str(error), file=sys.stderr)
        return 2
    observed = Counter(key(entry) for entry in findings)
    unexpected = observed - allowed
    if unexpected:
        for identity, count in sorted(unexpected.items()):
            tu, checker, file, function, message = identity
            print(f"{file}: {function}: {message} [{checker}] "
                  f"(translation unit {tu}; {count} unexpected)", file=sys.stderr)
        print(f"static analysis: FAIL ({sum(unexpected.values())} unexpected diagnostics)",
              file=sys.stderr)
        return 1
    stale = allowed - observed
    if stale:
        for identity, count in sorted(stale.items()):
            tu, checker, file, function, message = identity
            print(f"{file}: {function}: {message} [{checker}] "
                  f"(translation unit {tu}; {count} stale baseline occurrences)",
                  file=sys.stderr)
        print(f"static analysis: FAIL ({sum(stale.values())} stale baseline occurrences)",
              file=sys.stderr)
        return 1
    print(f"static analysis: clean ({len(findings)} baselined diagnostics, "
          "0 unexpected; 0 stale)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
