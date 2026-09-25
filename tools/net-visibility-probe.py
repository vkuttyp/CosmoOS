#!/usr/bin/env python3
"""
net-visibility-probe.py -- when the network refuses a guest's flows, what
can the operator see?

A guest that has spent its NAT share (NAT_QUOTA_PER_GUEST entries) has
every new outbound flow dropped. The kernel counts it (nat_stats'
out_drop_share; out_drop_full before the net-flows unit split it) and the
flows it holds sit in a table. This probe asks
what of that reaches a privileged operator on the machine, by making it
happen and comparing, before and after, everything an operator can read
about the network:

1. `/dev/net/tapctl`'s read snapshot -- the control plane's one listing
   (the probe reads it through the device's own read routine);
2. the kernel log (dmesg), from a marker line to the end;
3. the kernel's own counters and table occupancy, for contrast.

It patches `net-nat`'s quota step (kernel-services/network/nettest.c),
which already floods one test guest past its share, and adds a
probe-only wrapper around tap_ctl_read (kernel-services/network/tap.c).
The sysctl half needs no boot: `apply` prints every `net.` name the
native sysctl table serves.

    python3 tools/net-visibility-probe.py apply
    gmake ARCH=x86_64 test        # and ARCH=aarch64
    grep NVPROBE out/<arch>-debug/boot-test.log
    python3 tools/net-visibility-probe.py revert

Each boot prints:
    NVPROBE nat: entries A -> B (quota Q), out_drop_share +D
    NVPROBE tapctl: snapshot X bytes before, Y after, identical: yes|no
                    (yes before the net-flows unit; its flow section makes it no)
    NVPROBE dmesg: N lines between the mark and the end, K naming nat
                   (or INVALID if the log ring wrapped past the mark)

`apply` refuses a file with uncommitted changes (or a `git status` that
fails), refuses to overwrite a backup an earlier run left, and edits
nothing if an anchor is missing. It builds every patch in memory, writes
the stamp -- each file's original and patched hash -- and then replaces
each backup and file atomically, so from the stamp on every file is
exactly one of the two. `revert` accepts a file already original,
restores the patched ones atomically, removes the stamp, and only then the
backups, so an apply or revert interrupted anywhere is finished by running
revert (again).
"""

import hashlib
import os
import re
import shutil
import subprocess
import sys

NETTEST = 'kernel-services/network/nettest.c'
TAP = 'kernel-services/network/tap.c'
NATIVE = 'kernel/syscall/native.c'
BACKUP = '.net-visibility-probe.orig'
STAMP = '.net-visibility-probe.applied'

T_ANCHOR = 'static const struct chrdev_ops tap_ctl_ops = { .read = tap_ctl_read, .write = tap_ctl_write };\n'
T_PROBE = T_ANCHOR + '''
/* NVPROBE (tools/net-visibility-probe.py; not for merge): the operator's
 * listing, read the way the device reads it. */
int64_t nvprobe_tapctl_read(void *buf, size_t len);
int64_t nvprobe_tapctl_read(void *buf, size_t len)
{
    return tap_ctl_read(NULL, 0, buf, len);
}
'''

N_ANCHOR_BEFORE = '    for (unsigned i = 0; i < NAT_TABLE_SIZE + 8; i++) {\n'
N_PROBE_BEFORE = '''    /* NVPROBE: what an operator can read, before the flood. */
    static uint8_t nvp_a[COSMO_NETCTL_SNAPSHOT_MAX], nvp_b[COSMO_NETCTL_SNAPSHOT_MAX];
    int64_t nvp_na = nvprobe_tapctl_read(nvp_a, sizeof(nvp_a));
    kinfo("NVPROBE mark");
''' + N_ANCHOR_BEFORE

N_ANCHOR_AFTER = '    CHECK(ns1.out_drop_share > ns0.out_drop_share);      /* new flows dropped once the share is full */\n'
N_PROBE_AFTER = N_ANCHOR_AFTER + '''    {   /* NVPROBE: and after it. */
        int64_t nvp_nb = nvprobe_tapctl_read(nvp_b, sizeof(nvp_b));
        static char nvp_log[KLOG_RING_SIZE];
        size_t nl = klog_copy(nvp_log, sizeof(nvp_log) - 1);
        nvp_log[nl] = 0;
        const char *mark = NULL;
        for (const char *p = nvp_log; (p = strstr(p, "NVPROBE mark")) != NULL; p++)
            mark = p;
        unsigned lines = 0, nat_lines = 0;
        if (mark != NULL) {
            const char *p = strchr(mark, '\\n');
            while (p != NULL && p[1] != 0) {
                const char *line = p + 1;
                p = strchr(line, '\\n');
                size_t ll = p != NULL ? (size_t)(p - line) : strlen(line);
                lines++;
                for (size_t k = 0; k + 3 <= ll; k++)
                    if (line[k] == 'n' && line[k + 1] == 'a' && line[k + 2] == 't') { nat_lines++; break; }
            }
        }
        kinfo("NVPROBE nat: entries %u -> %u (quota %u), out_drop_share +%llu",
              ns0.entries, ns1.entries, (unsigned)NAT_QUOTA_PER_GUEST,
              (unsigned long long)(ns1.out_drop_share - ns0.out_drop_share));
        kinfo("NVPROBE tapctl: snapshot %lld bytes before, %lld after, identical: %s",
              (long long)nvp_na, (long long)nvp_nb,
              nvp_na == nvp_nb && nvp_na > 0 && memcmp(nvp_a, nvp_b, (size_t)nvp_na) == 0 ? "yes" : "no");
        if (mark == NULL)   /* the ring wrapped past it: no count is a measurement */
            kinfo("NVPROBE dmesg: INVALID -- the mark is gone from the ring, nothing measured");
        else
            kinfo("NVPROBE dmesg: %u lines between the mark and the end, %u naming nat",
                  lines, nat_lines);
    }
'''

N_ANCHOR_DECL = 'bool selftest_net_nat(const char **reason)\n'
N_PROBE_DECL = 'int64_t nvprobe_tapctl_read(void *buf, size_t len);   /* NVPROBE */\n' + N_ANCHOR_DECL


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
    for path, _, _ in entries:
        for leftover in (path + BACKUP, path + '.probe-tmp'):
            if os.path.exists(leftover):
                os.remove(leftover)
    print('reverted')


def files():
    return [
        (TAP, [(T_ANCHOR, T_PROBE)]),
        (NETTEST, [(N_ANCHOR_DECL, N_PROBE_DECL), (N_ANCHOR_BEFORE, N_PROBE_BEFORE),
                   (N_ANCHOR_AFTER, N_PROBE_AFTER)]),
    ]


def sysctl_net_names():
    s = open(NATIVE).read()
    m = re.search(r'static const char \*const sysctl_names\[\] = \{(.*?)\};', s, re.S)
    names = re.findall(r'"([^"]+)"', m.group(1)) if m else []
    return names, [n for n in names if n.startswith('net.')]


def apply():
    apply_files(files())
    names, net = sysctl_net_names()
    print(f'applied; sysctl serves {len(names)} names, {len(net)} under net.: {", ".join(net) or "none"}')


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
