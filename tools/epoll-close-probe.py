#!/usr/bin/env python3
"""Restore epoll's v1 lifetime -- no removal on a member's final close -- and show epoll-close fail.

Invariant A9: an epoll registration lives exactly as long as some
handle-table slot, in any process, holds its member; the last close removes
it, without EPOLL_CTL_DEL, as Linux drops an epitem at the file's final
close. Before 2026-10-08 the registration kept the member alive and the set
went on evaluating it: a closed TCP socket in an epoll set never sent its
FIN (docs/kernel/io/design.md, "epoll").

This probe builds a temporary clone with the removal restored to v1 (`--old`:
handle_close no longer tells epoll when the last slot empties) or as
committed, and boots to a panic right after the test carrying its verdict:
  python3 tools/epoll-close-probe.py --arch x86_64 --old   # must FAIL at the baseline: the registration still holds the closed pipe end
  python3 tools/epoll-close-probe.py --arch x86_64         # must PASS
AArch64 is the same with --arch aarch64. Four CPUs; never edits the
working tree (the clone is of HEAD: commit first); logs under
out/epoll-close-<arch>-{old,fixed}/.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise RuntimeError('probe anchor must occur exactly once: ' + before[:80])
    return source.replace(before, after)


def failing_check(tests):
    expr = 'kobject_refcount(rd) == 1'
    for i, l in enumerate(tests.splitlines()):
        if 'CHECK(' + expr + ');' in l and 'the baseline' in l:
            return expr, i + 1
    raise RuntimeError('the check is not in epolltest.c as expected')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--arch', choices=['x86_64', 'aarch64'], default='x86_64')
    ap.add_argument('--old', action='store_true', help='no removal on the final close, as in epoll v1')
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    tag = 'epoll-close-' + args.arch + ('-old' if args.old else '-fixed')
    out = root / 'out' / tag
    shutil.rmtree(out, ignore_errors=True)   # a previous run's dependency files name a temporary clone that is gone
    out.mkdir(parents=True, exist_ok=True)

    handle = (root / 'kernel/object/handle.c').read_text()
    if args.old:
        handle = replace_once(handle, '''    if (left == 0)
        epoll_last_handle_closed(obj);''', '''    (void)left;   /* EPOLLCLOSEPROBE --old: the registration outlives the last descriptor */''')
    tests = (root / 'kernel/io/epolltest.c').read_text()
    tests = replace_once(tests, '''bool selftest_epoll_close(const char **reason)
{''', '''static bool epollcloseprobe_body(const char **reason);
bool selftest_epoll_close(const char **reason)
{
    bool r = epollcloseprobe_body(reason);
    panic("EPOLLCLOSEPROBE: ok=%u reason=%s", r, *reason);
}
static bool epollcloseprobe_body(const char **reason)
{''')
    tests = replace_once(tests, '#include <kernel/object.h>\n', '#include <kernel/object.h>\n#include <kernel/panic.h>\n')

    with tempfile.TemporaryDirectory(prefix='cosmo-epoll-close-') as tmp:
        subprocess.run(['git', 'clone', '--quiet', '--shared', str(root), tmp], check=True)
        work = Path(tmp)
        (work / 'kernel/object/handle.c').write_text(handle)
        (work / 'kernel/io/epolltest.c').write_text(tests)
        with (out / 'build.log').open('w') as log:
            subprocess.run(['gmake', '-j4', 'ARCH=' + args.arch, 'BUILD=debug', 'HAVE_MUSL=0',
                            'OUT=' + str(out), 'image'], cwd=work, stdout=log, stderr=subprocess.STDOUT, check=True)
        spec = importlib.util.spec_from_file_location('epoll_close_boot', root / 'tests/boot/run_boot_test.py')
        harness = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(harness)
        if args.old:
            expr, line = failing_check(tests)
            outcome = r'ok=0 reason=check failed: ' + re.escape(expr) + ' at line %u' % line
        else:
            outcome = r'ok=1 reason='
        harness.PANIC_REQUIRED_MARKERS = [r'^KERNEL PANIC: EPOLLCLOSEPROBE: ' + outcome + '$']
        harness.PANIC_FORBIDDEN_MARKERS = [r'^SELFTEST: epoll-close ', r'KERNEL PANIC \(recursive\)']
        os.environ.update(COSMO_ARCH=args.arch, QEMU_ARCH=args.arch, QEMU_SMP='4')
        sys.argv = ['epoll-close-probe', '--expect-panic', 'fault', '--timeout', '170',
                    '--image', str(out / 'cosmoos.img'), '--log', str(out / 'boot.log')]
        return harness.main()


if __name__ == '__main__':
    sys.exit(main())
