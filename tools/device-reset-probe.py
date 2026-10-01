#!/usr/bin/env python3
"""device-reset-probe.py -- the device model has no generic reset operation.

`struct device_driver` has match/probe/remove and no `reset`, and there is no
`device_reset(dev)`. The only in-place re-initialization of a bound device is
a full remove + reprobe (pci_test_remove + pci_test_rebind), which replaces
the device and its higher-level object (a virtio-blk becomes a fresh blkdev).
The virtio layer has an in-place reset primitive (virtio_device_reset) but it
is reachable only inside each driver's own probe/remove, not as a model
operation. See docs/audit/next-subsystem-device-reset.md.

This probe adds a self-test, `device-reset-gap`, that re-initializes the
removal disk (QEMU_RMDISK) the only way the model allows -- remove+reprobe --
and reports that there is no in-place reset.

    python3 tools/device-reset-probe.py apply
    gmake ARCH=x86_64 QEMU_RMDISK=1 test > run.txt 2>&1   # or the default harness
    grep DEVRESET out/x86_64-debug/boot-test.log
    python3 tools/device-reset-probe.py revert

`apply` and `revert` are those of tools/lockup-interrupted-probe.py.
"""

import hashlib
import os
import stat
import subprocess
import sys

DEVTEST = 'kernel/device/devtest.c'
DECL = 'kernel/include/kernel/selftest.h'
REG = 'kernel/core/selftest.c'
BACKUP = '.device-reset-probe.orig'
STAMP = '.device-reset-probe.applied'

OLD_FN = """bool selftest_virtio_remove_inflight(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_virtio_remove_inflight_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}
"""
NEW_FN = OLD_FN + r"""
/* DEVRESET probe: the device model has no reset operation. The only in-place
 * re-init of a bound device is remove+reprobe, which replaces it. */
bool selftest_device_reset_gap(const char **reason)
{
#if !CONFIG_DEBUG
    (void)reason;
    kinfo("selftest: device-reset-gap: no test hooks in this build; skipping");
    return true;
#else
    struct blkdev *bd = rm_find();
    if (bd == NULL) {
        kinfo("selftest: device-reset-gap: no removal disk (QEMU_RMDISK=0); skipping");
        return true;
    }
    char name[BLKDEV_NAME_MAX];
    memcpy(name, bd->name, sizeof(name));
    struct pci_device *pdev = to_pci_device(to_virtio_device(bd->dev)->hw);
    uint8_t *pat = kmalloc(4096, 0), *chk = kmalloc(4096, 0);
    if (pat == NULL || chk == NULL) {
        kfree(pat);
        kfree(chk);
        blkdev_put(bd);
        *reason = "check failed: the probe's own buffers";
        return false;
    }
    for (unsigned i = 0; i < 512; i++)
        pat[i] = (uint8_t)(i * 7 + 3);
    bool ok = rm_io(bd, BIO_WRITE, pat) == 0;
    blkdev_put(bd);
    /* The only in-place re-init the model offers is a full remove + reprobe:
     * there is no device_reset op, and virtio_device_reset is private to the
     * driver's remove. */
    if (ok)
        ok = pci_test_remove(pdev) == 0 && pci_test_rebind(pdev) == 0;
    struct blkdev *again = ok ? rm_find() : NULL;
    if (again != NULL) {
        memset(chk, 0, 512);
        ok = strcmp(again->name, name) == 0 && rm_io(again, BIO_READ, chk) == 0 &&
             memcmp(chk, pat, 512) == 0;
        blkdev_put(again);
    } else {
        ok = false;
    }
    kfree(pat);
    kfree(chk);
    if (!ok) {
        if (pdev->dev.state == DEV_UNBOUND)
            pci_test_rebind(pdev);
        *reason = "check failed: the removal disk did not survive remove+reprobe";
        return false;
    }
    kprintf("DEVRESET: struct device_driver has no reset op; re-initialising %s needed remove+reprobe "
            "(a fresh blkdev), not an in-place reset; virtio_device_reset exists but is private to the "
            "driver's remove\n", name);
    return true;
#endif
}
"""

OLD_DECL = """bool selftest_virtio_remove_inflight(const char **reason);
"""
NEW_DECL = OLD_DECL + """bool selftest_device_reset_gap(const char **reason);
"""

OLD_REG = """    { "virtio-remove-inflight", selftest_virtio_remove_inflight },
"""
NEW_REG = OLD_REG + """    { "device-reset-gap", selftest_device_reset_gap },
"""


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def write_atomic(path, data):
    tmp = path + '.probe-tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    if os.path.exists(path):
        os.chmod(tmp, stat.S_IMODE(os.stat(path).st_mode))
    os.replace(tmp, path)


def git_clean(path):
    r = subprocess.run(['git', 'status', '--porcelain', '--', path], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f'git status failed for {path}: {r.stderr.strip() or r.returncode}')
    return not r.stdout.strip()


def apply_files(fl):
    if os.path.exists(STAMP):
        sys.exit('already applied (or an apply was interrupted): run revert first')
    if os.path.exists(STAMP + '.probe-tmp'):
        os.remove(STAMP + '.probe-tmp')
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
            os.remove(STAMP + '.probe-tmp')
            sys.exit('not applied (removed a partial stamp an interrupted apply left)')
        sys.exit('not applied')
    entries = [line.split() for line in open(STAMP).read().split('\n') if line]
    for path, patched, orig in entries:
        cur = sha(path)
        if cur == orig:
            continue
        if cur != patched:
            sys.exit(f'{path} changed since apply. {path + BACKUP} is the pre-probe '
                     f'original, so copying it back would erase those changes. Keep your '
                     f'edits and remove the DEVRESET lines by hand, then delete {path + BACKUP} '
                     f'and {STAMP}.')
        if not os.path.exists(path + BACKUP) or sha(path + BACKUP) != orig:
            sys.exit(f'{path} is still patched and {path + BACKUP} is missing or not its original; restore by hand')
    for path, patched, orig in entries:
        if sha(path) == patched:
            write_atomic(path, open(path + BACKUP, 'rb').read())
    for path, _, _ in entries:
        for leftover in (path + BACKUP, path + '.probe-tmp', path + BACKUP + '.probe-tmp'):
            if os.path.exists(leftover):
                os.remove(leftover)
    if os.path.exists(STAMP + '.probe-tmp'):
        os.remove(STAMP + '.probe-tmp')
    os.remove(STAMP)
    print('reverted')


def apply():
    apply_files([
        (DEVTEST, [(OLD_FN, NEW_FN)]),
        (DECL, [(OLD_DECL, NEW_DECL)]),
        (REG, [(OLD_REG, NEW_REG)]),
    ])
    print('applied: device-reset-gap shows the model has no in-place reset')


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
