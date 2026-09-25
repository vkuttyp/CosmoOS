#!/usr/bin/env python3
"""
net-visibility-probe.py -- when the network refuses a guest's flows, what
can the operator see?

A guest that has spent its NAT share (NAT_QUOTA_PER_GUEST entries) has
every new outbound flow dropped. The kernel counts it (nat_stats'
out_drop_full) and the flows it holds sit in a table. This probe asks
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
    NVPROBE nat: entries A -> B (quota Q), out_drop_full +D
    NVPROBE tapctl: snapshot X bytes before, Y after, identical: yes|no
    NVPROBE dmesg: N lines between the mark and the end, K naming nat

`apply` refuses a file with uncommitted changes, refuses to overwrite a
backup an earlier run left, and edits nothing if an anchor is missing,
restoring every file on a failure part-way; `revert` checks every file's
hash first (accepting one an earlier attempt already restored), restores
all from copies, and removes the backups only once every file is
restored.
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
    static uint8_t nvp_a[8192], nvp_b[8192];
    int64_t nvp_na = nvprobe_tapctl_read(nvp_a, sizeof(nvp_a));
    kinfo("NVPROBE mark");
''' + N_ANCHOR_BEFORE

N_ANCHOR_AFTER = '    CHECK(ns1.out_drop_full > ns0.out_drop_full);        /* new flows dropped once full */\n'
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
        kinfo("NVPROBE nat: entries %u -> %u (quota %u), out_drop_full +%llu",
              ns0.entries, ns1.entries, (unsigned)NAT_QUOTA_PER_GUEST,
              (unsigned long long)(ns1.out_drop_full - ns0.out_drop_full));
        kinfo("NVPROBE tapctl: snapshot %lld bytes before, %lld after, identical: %s",
              (long long)nvp_na, (long long)nvp_nb,
              nvp_na == nvp_nb && nvp_na > 0 && memcmp(nvp_a, nvp_b, (size_t)nvp_na) == 0 ? "yes" : "no");
        kinfo("NVPROBE dmesg: %u lines between the mark and the end, %u naming nat%s",
              lines, nat_lines, mark == NULL ? " (mark not found)" : "");
    }
'''

N_ANCHOR_DECL = 'bool selftest_net_nat(const char **reason)\n'
N_PROBE_DECL = 'int64_t nvprobe_tapctl_read(void *buf, size_t len);   /* NVPROBE */\n' + N_ANCHOR_DECL


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


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
    if os.path.exists(STAMP):
        sys.exit('already applied')
    fl = files()
    for path, edits in fl:
        if os.path.exists(path + BACKUP):
            sys.exit(f'{path + BACKUP} exists from an earlier run; restore or remove it by hand first')
        if not os.path.isfile(path):
            sys.exit(f'{path} not found: run from the top of the tree')
        if subprocess.run(['git', 'status', '--porcelain', '--', path], capture_output=True, text=True).stdout.strip():
            sys.exit(f'{path} has uncommitted changes')
        s = open(path).read()
        for a, _ in edits:
            if s.count(a) != 1:
                sys.exit(f'{path}: anchor not found exactly once: {a[:50]!r}')
    done, stamp = [], []
    try:
        for path, edits in fl:
            shutil.copyfile(path, path + BACKUP)
            done.append(path)
            s = open(path).read()
            for a, b in edits:
                s = s.replace(a, b)
            open(path, 'w').write(s)
            stamp.append(f'{path} {sha(path)}')
        open(STAMP, 'w').write('\n'.join(stamp) + '\n')
    except BaseException:
        for path in done:
            shutil.move(path + BACKUP, path)
            os.utime(path, None)
        if os.path.exists(STAMP):
            os.remove(STAMP)
        raise
    names, net = sysctl_net_names()
    print(f'applied; sysctl serves {len(names)} names, {len(net)} under net.: {", ".join(net) or "none"}')


def revert():
    if not os.path.exists(STAMP):
        sys.exit('not applied')
    paths = []
    for line in open(STAMP).read().split('\n'):
        if line:
            path, digest = line.split()
            paths.append(path)
            restored = os.path.exists(path + BACKUP) and sha(path) == sha(path + BACKUP)
            if sha(path) != digest and not restored:
                sys.exit(f'{path} changed since apply; restore by hand from {path + BACKUP}')
    for path in paths:
        shutil.copyfile(path + BACKUP, path)
        os.utime(path, None)
    for path in paths:
        os.remove(path + BACKUP)
    os.remove(STAMP)
    print('reverted')


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
