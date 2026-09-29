#!/usr/bin/env python3
"""
net-leftover-probe.py -- what does a network test leave behind, and why
did net-dns's expiry check fail?

One aarch64 boot (docs/testing/flakes.md, 2026-09-29):

    net-dns ... FAIL: check failed: s1.dns_pending == 0 && s1.dns_expired > s0.dns_expired at line 4817

and then six tests could not open /dev/net/tap, net-hoststate failed on
`svc != NULL`, and process-user failed. net-dns makes a tap, a DHCP/DNS
service, two sockets and a responder thread, and releases them only on
its last lines: every CHECK before them returns holding them all.

The probe:
- logs, whenever they change across a test, the live DHCP/DNS services,
  the network interfaces (by name) and the socket count (NLPROBE left);
- at net-dns's expiry step, logs the queries the service had handled
  when the test stopped waiting for the flood and at its check -- was
  the flood still draining? It adds no delay of its own (NLPROBE dns);
- registers net-dns REPS times (default 20);
- `--fail-dns` makes the expiry check fail, to see the cascade whole.

    python3 tools/net-leftover-probe.py apply [--fail-dns] [--slow-dns US] [--gap MS] [REPS]
    gmake ARCH=aarch64 test
    grep NLPROBE out/aarch64-debug/boot-test.log
    python3 tools/net-leftover-probe.py revert

`apply` and `revert` are those of tools/nvme-admin-probe.py: stamp first,
every file replaced atomically, the stamp removed last, finished by
running revert (again).
"""

import hashlib
import os
import subprocess
import sys

NETIF = 'kernel-services/network/netif.c'
TAPSVC = 'kernel-services/network/tapsvc.c'
NETTEST = 'kernel-services/network/nettest.c'
REG = 'kernel/core/selftest.c'
BACKUP = '.net-leftover-probe.orig'
STAMP = '.net-leftover-probe.applied'

NETIF_EDITS = [("""static LIST_HEAD(g_netifs);
static spinlock_t g_netif_lock = SPINLOCK_INIT("netifs");
""", """static LIST_HEAD(g_netifs);
static spinlock_t g_netif_lock = SPINLOCK_INIT("netifs");
/* NLPROBE: the interfaces, by name */
unsigned nlprobe_netifs(char *buf, size_t n);
unsigned nlprobe_netifs(char *buf, size_t n)
{
    unsigned count = 0;
    size_t off = 0;
    if (n)
        buf[0] = '\\0';
    arch_irq_state_t s = spin_lock_irqsave(&g_netif_lock);
    struct netif *it;
    list_for_each_entry(it, &g_netifs, link) {
        count++;
        if (off + 12 < n)
            off += (size_t)ksnprintf(buf + off, n - off, "%s%s", off ? "," : "", it->name);
    }
    spin_unlock_irqrestore(&g_netif_lock, s);
    return count;
}
""")]

TAPSVC_EDITS = [("""#define TAPSVC_MAX 8u
static struct tapsvc *g_svcs[TAPSVC_MAX];
""", """#define TAPSVC_MAX 8u
static struct tapsvc *g_svcs[TAPSVC_MAX];
unsigned nlprobe_svcs(void);   /* NLPROBE */
""")]

TAPSVC_TAIL = """
unsigned nlprobe_svcs(void)   /* NLPROBE: services holding a slot */
{
    unsigned n = 0;
    arch_irq_state_t s = spin_lock_irqsave(&g_svcs_lock);
    for (unsigned i = 0; i < TAPSVC_MAX; i++)
        if (g_svcs[i])
            n++;
    spin_unlock_irqrestore(&g_svcs_lock, s);
    return n;
}
"""

NETTEST_EDITS = [("""    tapsvc_dns_age(clock_now_ns() + 2ull * 5ull * NS_PER_SEC);
    tapsvc_get_stats(&s1);
    CHECK(s1.dns_expired - s0.dns_expired >= s0.dns_pending);""", """    tapsvc_dns_age(clock_now_ns() + 2ull * 5ull * NS_PER_SEC);
    tapsvc_get_stats(&s1);
    /* NLPROBE: no delay of its own -- a sleep here is the settle the test
     * lacks, and hid the failure. Queries handled between the wait's end
     * (s0) and the check (s1): the flood still draining. */
    kinfo("NLPROBE dns: queries handled %llu at the settle, %llu at the check (+%llu); pending %u then %u; expired +%llu",
          (unsigned long long)s0.dns_query, (unsigned long long)s1.dns_query,
          (unsigned long long)(s1.dns_query - s0.dns_query), s0.dns_pending, s1.dns_pending,
          (unsigned long long)(s1.dns_expired - s0.dns_expired));
    CHECK(s1.dns_expired - s0.dns_expired >= s0.dns_pending);""")]


def slow_dns(us):
    # --slow-dns US: the guest-side DNS thread takes US microseconds per
    # query, as a loaded host makes it -- the flood is still queued when the
    # test's wait (the first drop) ends. A busy wait, not a sleep: a sleep
    # lasts until a later tick, the socket overflows, and the table never
    # fills.
    return ('''        int64_t n = ksock_recvfrom(svc->gsock, buf, sizeof(buf), &from);
        if (n < DNS_HDR) { if (n <= 0) break; continue; }''', f'''        int64_t n = ksock_recvfrom(svc->gsock, buf, sizeof(buf), &from);
        if (n < DNS_HDR) {{ if (n <= 0) break; continue; }}
        {{   /* NLPROBE --slow-dns */
            uint64_t nl_t0 = clock_now_ns();
            while (clock_since_ns(nl_t0) < {us}ull * 1000ull)
                arch_cpu_relax();
        }}''')


def gap(ms):
    # --gap MS: a pause between the age and the check, as a host that
    # deschedules the test's vCPU there makes one. With the flood still
    # draining (--slow-dns), a query lands in it.
    return ("""    tapsvc_dns_age(clock_now_ns() + 2ull * 5ull * NS_PER_SEC);
    tapsvc_get_stats(&s1);""", f"""    tapsvc_dns_age(clock_now_ns() + 2ull * 5ull * NS_PER_SEC);
    thread_sleep_ms({ms});   /* NLPROBE --gap */
    tapsvc_get_stats(&s1);""")


FAIL_DNS = ("""    CHECK(s1.dns_expired - s0.dns_expired >= s0.dns_pending);""",
            """    CHECK(s1.dns_pending == 0 && s1.dns_expired > s0.dns_expired && !"NLPROBE --fail-dns");""")


# --- the implementation's verification (the net-leftover unit) -----------
#
# --count-checks: every CHECK, CHECK_BREAK and TCHECK in nettest.c counts
# the checks a test passes, and the runner logs the count per test
# (NLPROBE checks). --force last|mid TABLE: from such a log, each test in
# it fails at its last passed check (or halfway), in one boot -- the
# runner must release what it held (no "left the network" line), nothing
# may hang, and every other test must pass.

NLP_RUNNER = [('''        g_defer_overflow = false;
        bool ok = tests[i].fn(&reason);''', '''        g_defer_overflow = false;
        nlp_checks = 0;
        nlp_forced = false;
        nlp_force_at = 0;
        for (unsigned q = 0; q < sizeof(nlp_table) / sizeof(nlp_table[0]); q++)
            if (nlp_table[q].name && strcmp(nlp_table[q].name, tests[i].name) == 0)
                nlp_force_at = nlp_table[q].at;
        bool ok = tests[i].fn(&reason);
        if (nlp_checks != 0 && !nlp_forced)
            kinfo("NLPROBE checks '%s' %u", tests[i].name, nlp_checks);
        if (nlp_forced)
            kinfo("NLPROBE forced '%s' at check %u of its passing ones", tests[i].name, nlp_force_at);''')]

def nlp_globals(table):
    rows = ''.join(f'    {{ "{n}", {at} }},\n' for n, at in table) or '    { NULL, 0 },\n'
    return ('''int selftest_run_all(void)
{''', '''/* NLPROBE: the forced-failure table, and the counters the checks bump */
unsigned nlp_checks, nlp_force_at;
bool nlp_forced;
static const struct { const char *name; unsigned at; } nlp_table[] = {
''' + rows + '''};

int selftest_run_all(void)
{''')

NLP_MACROS = [
    ('''#define CHECK(cond)                                                            \\
    do {                                                                       \\
        if (!(cond)) {                                                         \\
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \\
            return false;                                                      \\
        }                                                                      \\
    } while (0)''', '''extern unsigned nlp_checks, nlp_force_at;   /* NLPROBE */
extern bool nlp_forced;
static inline bool nlp_hit(void)
{
    if (++nlp_checks == nlp_force_at) {
        nlp_forced = true;
        return true;
    }
    return false;
}
#define CHECK(cond)                                                            \\
    do {                                                                       \\
        if (!(cond) || nlp_hit()) {                                            \\
            *reason = nlp_forced ? "NLPROBE forced" : "check failed: " #cond " at line " STR(__LINE__); \\
            return false;                                                      \\
        }                                                                      \\
    } while (0)'''),
    ('''#define CHECK_BREAK(cond)                                                      \\
    if (!(cond)) {                                                             \\''', '''#define CHECK_BREAK(cond)                                                      \\
    if (!(cond) || nlp_hit()) {                                                \\'''),
    ('''        if (!(cond)) {                                                                       \\
            *reason = "tap-ready: " #cond;                                                   \\''', '''        if (!(cond) || nlp_hit()) {                                                          \\
            *reason = "tap-ready: " #cond;                                                   \\'''),
]


def reg_edits(reps):
    return [
        ("""        bool ok = tests[i].fn(&reason);
""", """        bool ok = tests[i].fn(&reason);
        {   /* NLPROBE: what the network is left holding */
            extern unsigned nlprobe_netifs(char *buf, size_t n);
            extern unsigned nlprobe_svcs(void);
            extern unsigned socket_count(void);
            static unsigned last_n = ~0u, last_s = ~0u, last_k = ~0u;
            static char last_names[160];
            char names[160];
            unsigned n = nlprobe_netifs(names, sizeof(names)), sv = nlprobe_svcs(), k = socket_count();
            /* The names too: a test that removes one interface and adds
             * another leaves the count where it was. */
            if (n != last_n || sv != last_s || k != last_k || strcmp(names, last_names) != 0)
                kinfo("NLPROBE left: '%s' ended with %u netifs (%s), %u services, %u sockets",
                      tests[i].name, n, names, sv, k);
            last_n = n;
            last_s = sv;
            last_k = k;
            strlcpy(last_names, names, sizeof(last_names));
        }
"""),
        ('    { "net-dns",         selftest_net_dns },\n', '    { "net-dns",         selftest_net_dns },\n' * reps),
    ]


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


def apply_verify(args):
    count = '--count-checks' in args
    force = None
    if '--force' in args:
        k = args.index('--force')
        if k + 2 >= len(args) + 1 or args[k + 1] not in ('last', 'mid'):
            sys.exit('usage: apply --count-checks | --force last|mid LOGFILE')
        mode, log = args[k + 1], args[k + 2]
        table = []
        import re as _re
        for line in open(log, errors='replace'):
            m = _re.search(r"NLPROBE checks '([^']+)' (\d+)", line)
            if m and int(m.group(2)) > 0:
                n = int(m.group(2))
                table.append((m.group(1), n if mode == 'last' else max(1, n // 2)))
        force = table
    table = force or []
    apply_files([(REG, [nlp_globals(table)] + NLP_RUNNER), (NETTEST, NLP_MACROS)])
    print(f'applied: checks counted per test' + (f'; {len(table)} tests forced to fail ({args[args.index("--force") + 1]})' if force else ''))


def apply():
    args = sys.argv[2:]
    if '--count-checks' in args or '--force' in args:
        return apply_verify(args)
    fail = '--fail-dns' in args
    args = [a for a in args if a != '--fail-dns']
    slow = None
    if '--slow-dns' in args:
        k = args.index('--slow-dns')
        if k + 1 >= len(args) or not args[k + 1].isdigit():
            sys.exit('usage: apply [--fail-dns] [--slow-dns US] [REPS]')
        slow = int(args[k + 1])
        del args[k:k + 2]
    gap_ms = None
    if '--gap' in args:
        k = args.index('--gap')
        if k + 1 >= len(args) or not args[k + 1].isdigit():
            sys.exit('usage: apply [--fail-dns] [--slow-dns US] [--gap MS] [REPS]')
        gap_ms = int(args[k + 1])
        del args[k:k + 2]
    if any(not a.isdigit() for a in args) or len(args) > 1:
        sys.exit('usage: apply [--fail-dns] [--slow-dns US] [--gap MS] [REPS]')
    reps = int(args[0]) if args else 20
    tail_anchor = "    out->dns_pending = live;\n}\n"   # tapsvc_get_stats' end: the file's last function
    apply_files([(NETIF, NETIF_EDITS),
                 (TAPSVC, TAPSVC_EDITS + [(tail_anchor, tail_anchor + TAPSVC_TAIL)]
                  + ([slow_dns(slow), ('#include <kernel/net/tapsvc.h>\n', '#include <kernel/net/tapsvc.h>\n#include <arch/cpu.h>   /* NLPROBE --slow-dns */\n')]
                     if slow is not None else [])),
                 (NETTEST, NETTEST_EDITS + ([FAIL_DNS] if fail else []) + ([gap(gap_ms)] if gap_ms is not None else [])),
                 (REG, reg_edits(reps))])
    print(f'applied: network leftovers logged per test, net-dns registered {reps} times'
          + ('; its expiry check forced to fail' if fail else '')
          + (f'; the guest DNS thread takes {slow} us a query' if slow is not None else '')
          + (f'; {gap_ms} ms between the age and the check' if gap_ms is not None else ''))


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
