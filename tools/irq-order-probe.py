#!/usr/bin/env python3
"""
irq-order-probe.py -- which vGIC self-test assertions depend on where the
guest is when an interrupt is delivered?

Sightings (docs/testing/flakes.md): `el2-guest-irq-queue` on aarch64 CI's
GIC boot, four times. The fourth, after the first half of the test was
fixed, was named by the test's own instrument:

    selftest: hv: line 1239: expected hypercall 2, got exit kind 4 hypercall nr 5

The guest took INTID 5 before its heartbeat. The mechanism: the host
refills the one list register only at vCPU entry (el2_vdist_offer; no
maintenance interrupt), and vcpu_run re-enters after any exit it handles
itself -- a host interrupt (HV_EXIT_INTR) or the guest's own GIC access
(HV_EXIT_EMULATED) -- offering the lowest pending vector. So when a host
interrupt lands between the guest's EOI and its heartbeat hypercall, a
pending interrupt is delivered first. That ordering is a race with the
host's tick, and a test that asserts either order is asserting it.

The probe makes that exit certain instead of a race: it adds two
instructions to the guest's IRQ handler (tests/hv/aarch64/guest_irq.S),
after the EOI and before `eret` -- a read of GICD_CTLR, which is emulated
at EL2 and handed back to vcpu_run as HV_EXIT_EMULATED. Every assertion
that holds only when the heartbeat comes first then fails, deterministically,
at its own line; every assertion that does not depend on the order passes.

    python3 tools/irq-order-probe.py apply
    gmake ARCH=aarch64 test-gic      # the vGIC tests run in the GIC boot
    python3 tools/irq-order-probe.py revert

The result is the boot's own failure lines: `selftest: hv: line N: expected
hypercall 2, got ... hypercall nr M`. A test stops at its first failure, so
the report's sweep also reads each test by hand.

`apply` and `revert` are those of tools/nvme-admin-probe.py: stamp first,
every file replaced atomically, finished by running revert (again).
"""

import hashlib
import os
import shutil
import subprocess
import sys

GUEST = 'tests/hv/aarch64/guest_irq.S'
BACKUP = '.irq-order-probe.orig'
STAMP = '.irq-order-probe.applied'

G_ANCHOR = """    msr  icc_eoir1_el1, x0
    eret"""
G_PROBE = """    msr  icc_eoir1_el1, x0
    movz x1, #0x0800, lsl #16      /* IOPROBE (tools/irq-order-probe.py; not for merge): */
    ldr  w1, [x1]                  /* GICD_CTLR, emulated: an exit between the EOI and the heartbeat */
    eret"""


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def write_atomic(path, data):
    # Whole or not at all: a probe interrupted mid-write must leave every
    # file either as it was or as intended, never half of each.
    tmp = path + '.probe-tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def git_clean(path):
    r = subprocess.run(['git', 'status', '--porcelain', '--', path], capture_output=True, text=True)
    if r.returncode != 0:     # a failed check is not a clean tree
        sys.exit(f'git status failed for {path}: {r.stderr.strip() or r.returncode}')
    return not r.stdout.strip()


def apply_files(fl):
    """Patch every file in `fl`, or none. Every patch is built in memory
    first; then the stamp is written, recording each file's original and
    patched hash; then each backup and each file is replaced atomically.
    From the stamp on, every file is exactly its original or its patched
    bytes, so revert can finish whatever an interruption left."""
    if os.path.exists(STAMP):
        sys.exit('already applied (or an apply was interrupted): run revert first')
    if os.path.exists(STAMP + '.probe-tmp'):
        os.remove(STAMP + '.probe-tmp')  # the stamp is written first: without it nothing was patched
    plan = []
    for path, edits in fl:
        if os.path.exists(path + BACKUP):
            sys.exit(f'{path + BACKUP} exists from an earlier run; restore or remove it by hand first')
        if not os.path.isfile(path):
            sys.exit(f'{path} not found: run from the top of the tree')
        if not git_clean(path):
            sys.exit(f'{path} has uncommitted changes')
        orig = open(path, 'rb').read()
        s = orig.decode()
        for a, b in edits:
            if s.count(a) != 1:
                sys.exit(f'{path}: anchor not found exactly once: {a[:50]!r}')
            s = s.replace(a, b)
        plan.append((path, orig, s.encode()))
    write_atomic(STAMP, ''.join(f'{p} {hashlib.sha256(n).hexdigest()} {hashlib.sha256(o).hexdigest()}\n'
                                for p, o, n in plan).encode())
    for path, orig, new in plan:
        write_atomic(path + BACKUP, orig)
        write_atomic(path, new)


def revert():
    if not os.path.exists(STAMP):
        if os.path.exists(STAMP + '.probe-tmp'):
            os.remove(STAMP + '.probe-tmp')   # an apply interrupted before its stamp: nothing was patched
            sys.exit('not applied (removed a partial stamp an interrupted apply left)')
        sys.exit('not applied')
    entries = [line.split() for line in open(STAMP).read().split('\n') if line]
    for path, patched, orig in entries:
        cur = sha(path)
        if cur == orig:
            continue                     # never patched, or already restored
        if cur != patched:
            sys.exit(f'{path} changed since apply; restore by hand from {path + BACKUP}')
        if not os.path.exists(path + BACKUP) or sha(path + BACKUP) != orig:
            sys.exit(f'{path} is still patched and {path + BACKUP} is missing or not its original; restore by hand')
    for path, patched, orig in entries:
        if sha(path) == patched:
            write_atomic(path, open(path + BACKUP, 'rb').read())
    os.remove(STAMP)                     # every file is original now
    # Every temp write_atomic can leave: a file's, its backup's, the stamp's.
    for path, _, _ in entries:
        for leftover in (path + BACKUP, path + '.probe-tmp', path + BACKUP + '.probe-tmp'):
            if os.path.exists(leftover):
                os.remove(leftover)
    if os.path.exists(STAMP + '.probe-tmp'):
        os.remove(STAMP + '.probe-tmp')
    print('reverted')


def files():
    return [(GUEST, [(G_ANCHOR, G_PROBE)])]


def apply():
    apply_files(files())
    print('applied: the guest reads GICD_CTLR after every EOI')


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
