#!/usr/bin/env python3
"""Reproduce virtio-net removal defects in a throwaway worktree.

--old restores main 561b913b's remove body, preserving the debug ledger
and adapting only the TX cookie representation to the private record.
The probe requires the exact tests' failures; an unrelated failed boot is
never evidence. Without --old it requires both tests and the full boot to
pass. Logs and build output stay under out/vnet-remove-probe/.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--arch', required=True, choices=('x86_64', 'aarch64'))
    ap.add_argument('--old', action='store_true')
    ap.add_argument('--tree', default='HEAD', help='commit with the tests to build')
    args = ap.parse_args()
    work = ROOT / 'out' / 'vnet-remove-probe' / (args.arch + ('-old' if args.old else '-fixed'))
    work.mkdir(parents=True, exist_ok=True)
    tree = work / 'tree'
    if tree.exists():
        raise SystemExit(f'refusing to overwrite existing worktree {tree}')
    subprocess.run(['git', '-C', str(ROOT), 'worktree', 'add', '--detach', str(tree), args.tree], check=True)
    try:
        if args.old:
            path = tree / 'drivers/virtio/virtio_net.c'
            text = path.read_text()
            original = subprocess.check_output(['git', '-C', str(ROOT), 'show',
                '561b913b:drivers/virtio/virtio_net.c'], text=True)
            pat = r'static void vnet_remove\(struct virtio_device \*vdev\)\n\{.*?\n\}'
            old = re.search(pat, original, re.S).group()
            old = old.replace('m_freem(m);', 'vnet_free_mbuf(v, m);')
            # Ownership/order stay old: discard only used TX entries,
            # without unmapping. Translate the new private cookie to mbuf.
            old = old.replace('while ((m = virtq_pop(v->tx, &len)) != NULL)',
                'struct vnet_tx *tx;\n    while ((tx = virtq_pop(v->tx, &len)) != NULL)')
            old = old.replace('while ((tx = virtq_pop(v->tx, &len)) != NULL)\n        vnet_free_mbuf(v, m);',
                'while ((tx = virtq_pop(v->tx, &len)) != NULL)\n        vnet_free_mbuf(v, tx->m);')
            assert 'vnet_free_mbuf(v, tx->m);' in old, 'old TX cookie anchor changed'
            text, count = re.subn(pat, lambda m: old, text, count=1, flags=re.S)
            assert count == 1, 'remove anchor changed'
            path.write_text(text)
        make = 'gmake' if sys.platform == 'darwin' else 'make'
        command = [make, '-C', str(tree), 'ARCH=' + args.arch]
        with (work / 'build.log').open('w') as output:
            rc = subprocess.run(command + ['-j6', 'image'], stdout=output, stderr=subprocess.STDOUT).returncode
        if rc:
            print(f'PROBE: FAIL (build, {work / "build.log"})')
            return 1
        serial = work / 'boot.serial'
        with (work / 'boot.result').open('w') as output:
            rc = subprocess.run(command + ['BOOT_LOG=' + str(serial), 'test'], stdout=output,
                                stderr=subprocess.STDOUT).returncode
        log = serial.read_text(errors='replace') if serial.exists() else ''
        tests = ('vnet-remove-pending', 'vnet-remove-late')
        for line in log.splitlines():
            if 'selftest: vnet-remove:' in line or any(line.startswith('SELFTEST: ' + name) for name in tests):
                print(line)
        if args.old:
            wanted = ('mapped buffers not unmapped', 'receive buffers reposted after reset')
            ok = all(re.search(r'^SELFTEST: ' + name + r'\s+\.\.\. FAIL: vnet-remove: ' + why,
                               log, re.M) for name, why in zip(tests, wanted)) and rc != 0
            failures = re.findall(r'^SELFTEST: (\S+)\s+\.\.\. FAIL', log, re.M)
            ok = ok and len(failures) == 2 and set(failures) == set(tests)
            ok = ok and re.search(r'^SELFTEST: FAIL \(2 of \d+\)', log, re.M) is not None
            ok = ok and re.search(r'^SELFTEST: net-harness\s+\.\.\. ok', log, re.M) is not None
            ok = ok and 'shutdown: exit status 1' in log
            # Only the expected self-test verdict may fail the full harness.
            result = (work / 'boot.result').read_text(errors='replace')
            reasons = re.findall(r'^  - (.+)$', result, re.M)
            ok = ok and len(reasons) == 3 and reasons[0] == 'kernel reported failure via debug-exit'
            ok = ok and reasons[1].startswith('forbidden marker /SELFTEST: FAIL/: SELFTEST: FAIL (2 of ')
            ok = ok and reasons[2] == "no 'SELFTEST: PASS' line"
        else:
            ok = rc == 0 and all(re.search(r'^SELFTEST: ' + name + r'\s+\.\.\. ok', log, re.M)
                                 for name in tests)
        print(f'PROBE: {"PASS" if ok else "FAIL"} ({work})')
        return 0 if ok else 1
    finally:
        subprocess.run(['git', '-C', str(ROOT), 'worktree', 'remove', '--force', str(tree)], check=True)


if __name__ == '__main__':
    sys.exit(main())
