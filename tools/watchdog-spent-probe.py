#!/usr/bin/env python3
"""
watchdog-spent-probe.py -- does the self-test runner's hang watchdog still
watch the tests after `cosmofs-replay`?

`selftest_run_all` arms the scheduler's hang watchdog once, at 8 s, and
kicks it before each test. The watchdog prints its dump (every run queue,
every thread, a sample of every CPU) at most once per arming: the
`g_watchdog_fired` latch is cleared only by `sched_watchdog_arm`. A kick
does not clear it.
`cosmofs-replay` runs 13-19 s without a kick, by design (410 prefix
images behind one line). So every debug boot prints the dump inside a
passing test and then carries a spent watchdog through the 229 tests
registered after it.

The probe adds one self-test, `wdprobe-sleeper`, that sleeps 9 s without a
kick: a test that stops making progress for longer than the period.

    python3 tools/watchdog-spent-probe.py apply --late|--early
    gmake ARCH=x86_64 test > run.txt 2>&1   # the harness fails the sleeper's budget
    python3 tools/watchdog-spent-probe.py read out/x86_64-debug/boot-test.log
    python3 tools/watchdog-spent-probe.py revert

--late registers the sleeper just before `syscall-fuzz`, after
`cosmofs-replay`, where the five over-budget sightings in
docs/testing/flakes.md ran. --early registers it just before
`cosmofs-replay`. That is the control: the same sleeper, differing only in
position.

The report's candidate (then `--fix`, now built) armed the watchdog
afresh before each test, instead of kicking it, which clears the latch. The period is the
test's own budget: the harness's 8 s, or its composite budget for
`process-user` (20 s) and `cosmofs-replay` (40 s). A test that goes
quiet for longer than it is allowed to take gets the dump. A passing
composite test does not.

**Built (the watchdog-spent unit).** The runner now arms the watchdog per
test at the test's budget, so the tree is the fix and there is no `--fix`
(an old command line that passes it is told so).
On the built tree the sleeper also trips the harness's `[WATCHDOG]`
forbidden marker, beside its budget.

`read` names the test each `[WATCHDOG]` dump fell in: the first
`SELFTEST:` line after the dump.

`apply` and `revert` are those of tools/cond-phase-probe.py: stamp first,
every file replaced atomically, the stamp removed last, finished by
running revert (again).
"""

import hashlib
import os
import re
import stat
import subprocess
import sys

SELFTEST = 'kernel/core/selftest.c'
BACKUP = '.watchdog-spent-probe.orig'
STAMP = '.watchdog-spent-probe.applied'

SLEEP_MS = 9000

OLD_TABLE = """static const struct selftest tests[] = {"""
NEW_TABLE = """/* WDPROBE: a test that makes no progress for longer than the watchdog's period. */
static bool wdprobe_sleeper(const char **reason)
{
    (void)reason;
    kprintf("WDPROBE: sleeper begins: %u ms without a kick\\n", %uu);
    thread_sleep_ms(%uu);
    kprintf("WDPROBE: sleeper ends\\n");
    return true;
}

static const struct selftest tests[] = {""".replace('%uu', str(SLEEP_MS) + 'u')

OLD_EARLY = """    { "cosmofs-replay",  selftest_cosmofs_replay },"""
NEW_EARLY = """    { "wdprobe-sleeper", wdprobe_sleeper },   /* WDPROBE --early */
    { "cosmofs-replay",  selftest_cosmofs_replay },"""

OLD_LATE = """    { "syscall-fuzz",    selftest_syscall_fuzz },"""
NEW_LATE = """    { "wdprobe-sleeper", wdprobe_sleeper },   /* WDPROBE --late */
    { "syscall-fuzz",    selftest_syscall_fuzz },"""


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
    if os.path.exists(path):   # keep the file's mode: the umask must not change it
        os.chmod(tmp, stat.S_IMODE(os.stat(path).st_mode))
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
    # Every file is original now. The backups and every temp write_atomic can
    # leave go first and the stamp last: while the stamp exists a revert can
    # be run again and finish, and once it is gone nothing is left behind.
    for path, _, _ in entries:
        for leftover in (path + BACKUP, path + '.probe-tmp', path + BACKUP + '.probe-tmp'):
            if os.path.exists(leftover):
                os.remove(leftover)
    if os.path.exists(STAMP + '.probe-tmp'):
        os.remove(STAMP + '.probe-tmp')
    os.remove(STAMP)
    print('reverted')


def apply():
    args = sys.argv[2:]
    if '--fix' in args:
        sys.exit('--fix is built: the runner arms the watchdog per test at its budget (selftest_run_all)')
    late, early = '--late' in args, '--early' in args
    if late == early or [a for a in args if a not in ('--late', '--early')]:
        sys.exit('usage: apply --late|--early')
    edits = [(OLD_TABLE, NEW_TABLE), (OLD_LATE, NEW_LATE) if late else (OLD_EARLY, NEW_EARLY)]
    apply_files([(SELFTEST, edits)])
    print(f'applied: a {SLEEP_MS} ms sleeper '
          + ('after cosmofs-replay (before syscall-fuzz)' if late else 'before cosmofs-replay'))


DUMP = re.compile(r'^\[WATCHDOG\] no progress for (\d+) ms')
TEST = re.compile(r'^SELFTEST: (\S+)\s+\.\.\. (ok|FAIL.*?) \((\d+) ms\)')


def read():
    if len(sys.argv) != 3:
        sys.exit('usage: read <boot-test.log>')
    lines = open(sys.argv[2], encoding='utf-8', errors='replace').read().split('\n')
    pending = []        # dumps not yet placed: their "no progress" figure
    dumps = []          # (test, its duration, no progress ms)
    ntests = 0
    for ln in lines:
        m = DUMP.match(ln)
        if m:
            pending.append(int(m.group(1)))
            continue
        m = TEST.match(ln)
        if m:
            ntests += 1
            for p in pending:
                dumps.append((m.group(1), int(m.group(3)), p, ntests))
            pending = []
    print(f'WDPROBE: {ntests} self-tests, {len(dumps) + len(pending)} watchdog dumps')
    for name, ms, p, idx in dumps:
        print(f'WDPROBE: dump in test {idx} {name} ({ms} ms): no progress for {p} ms')
    for p in pending:
        print(f'WDPROBE: dump after the last self-test: no progress for {p} ms')
    for ln in lines:
        if ln.startswith('WDPROBE: sleeper') or TEST.match(ln) and 'wdprobe' in ln:
            print(ln.strip())


if __name__ == '__main__':
    {'apply': apply, 'revert': revert, 'read': read}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                                         lambda: sys.exit(__doc__))()
