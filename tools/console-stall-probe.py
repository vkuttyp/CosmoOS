#!/usr/bin/env python3
"""
console-stall-probe.py -- what stops when the aarch64 console goes quiet mid-line?

Three sightings (docs/testing/flakes.md, "An aarch64 release boot whose
console stopped mid-line after an interrupt"): in the aarch64 release
boot, right after a job event -- a ^C'd `sleep` reaped, a background
`sleep` finishing -- the echo of the next line the harness types stops
partway (`cosmo$ echo after-interru`), and nothing more comes for the
rest of the boot. Echo is the terminal's work, not the shell's, so either
the serial receive path stopped delivering or the guest stopped.

This probe asks which, by making the event happen many times and, when a
stall comes, looking at the guest from outside:

1. It injects into `tests/boot/shelltest.py` a prefix of CYCLES cycles
   of the two shapes seen: `sleep 1`, ^C after 0.5 s, then a typed line;
   and `sleep 1 &`, a pause timed so the job exits around the moment the
   next line arrives, then a long typed line.
2. When a prompt does not come, before giving up it records whether the
   console's input still works -- it sends Enter and ^C, waits three
   seconds, and looks for the terminal's echo of the ^C -- and asks QEMU, over QMP, for every vCPU's
   registers (`info registers -a`), which it writes beside the log.
3. `symbolize` turns the program counters in that dump into function
   names against the kernel's ELF.

The run_boot_test.py half sets QEMU_QMP for the shell harness's boots so
the socket exists. Usage:

    python3 tools/console-stall-probe.py apply [CYCLES]      # default 25
    gmake ARCH=aarch64 BUILD=release test                    # repeat as needed
    python3 tools/console-stall-probe.py symbolize out/aarch64-release/boot-test.log.regs out/aarch64-release/kernel/kernel.elf
    python3 tools/console-stall-probe.py revert

Each boot prints `CSPROBE:` lines into the harness's failure list and the
log: `CSPROBE stall at command K: ^C echoed after the stall: yes|no
(log activity +N bytes)` -- the echo, not mere log growth, is the
answer, since a background job's message can land in the window -- and `CSPROBE registers written to <path>`. A boot with no stall
passes, having run every cycle.

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

SHELL = 'tests/boot/shelltest.py'
BOOT = 'tests/boot/run_boot_test.py'
BACKUP = '.console-stall-probe.orig'
STAMP = '.console-stall-probe.applied'

S_ANCHOR_CMDS = 'COMMANDS = [\n'
S_ANCHOR_FAIL = '''                if not self._wait_prompt(log_path, proc, deadline, prompts):
                    self.error = f"no prompt before command {prompts} ({cmd!r})"
                    return'''
S_PROBE_FAIL = '''                if not self._wait_prompt(log_path, proc, deadline, prompts):
                    self.error = f"no prompt before command {prompts} ({cmd!r})"
                    self._csprobe(log_path, proc, prompts)   # CSPROBE
                    return'''
S_ANCHOR_FAILURES = '''    def failures(self, lines):
        out = []
        if self.error:
            out.append(f"shell harness: {self.error}")'''
S_PROBE_FAILURES = '''    def _csprobe(self, log_path, proc, k):   # CSPROBE: does the guest still answer, and where is every vCPU?
        import json, os, socket
        notes = self.results.setdefault("csprobe", [])
        try:
            before = os.path.getsize(log_path)
        except OSError:
            before = 0
        try:
            proc.stdin.write(b"\\r")
            proc.stdin.flush()
            time.sleep(1.0)
            proc.stdin.write(b"\\x03")
            proc.stdin.flush()
        except OSError:
            pass
        time.sleep(3.0)
        # Growth alone is not an answer: a background job's message can land
        # in this window while input stays stalled (found in review). The
        # terminal echoes ^C as "^C", so that echo is what says the input
        # path took the keystroke.
        try:
            with open(log_path, "rb") as lf:
                lf.seek(before)
                new = lf.read()
        except OSError:
            new = b""
        notes.append(f"CSPROBE stall at command {k}: ^C echoed after the stall: {'yes' if b'^C' in new else 'no'}"
                     f" (log activity +{len(new)} bytes)")
        path = os.environ.get("QEMU_QMP")
        if not path:
            notes.append("CSPROBE no QMP socket")
            return
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(10)
            s.connect(path)
            f = s.makefile("rwb")
            f.readline()   # greeting
            # The registers twice, half a second apart (spinning or idle);
            # then, on aarch64's virt machine, the console PL011's status
            # registers -- never its data register, which a read would pop:
            # FR (0x18; bit 4 RXFE: receive FIFO empty), CR (0x30), IMSC
            # (0x38; bits 4-6: RX, RT and TX interrupts enabled), RIS (0x3c)
            # and MIS (0x40) -- and the interrupt controller's view.
            hmp = ["info registers -a", "info registers -a"]
            if os.environ.get("COSMO_ARCH", "aarch64") == "aarch64":
                hmp += ["xp /1wx 0x09000018", "xp /1wx 0x09000030", "xp /1wx 0x09000038",
                        "xp /1wx 0x0900003c", "xp /1wx 0x09000040", "info pic"]
            for msg in [{"execute": "qmp_capabilities"}] + [
                    {"execute": "human-monitor-command", "arguments": {"command-line": c}} for c in hmp]:
                f.write((json.dumps(msg) + "\\n").encode())
                f.flush()
                while True:
                    reply = json.loads(f.readline())
                    if "event" not in reply:
                        break
                if msg["execute"] == "human-monitor-command":
                    out = log_path + ".regs"
                    cl = msg["arguments"]["command-line"]
                    with open(out, "a") as o:
                        o.write(("==== sample\\n" if cl == "info registers -a" else f"==== {cl}\\n")
                                + reply.get("return", str(reply)) + "\\n")
                    if cl == "info registers -a":
                        time.sleep(0.5)   # two samples, half a second apart: spinning or idle
            s.close()
            notes.append(f"CSPROBE registers written to {log_path}.regs")
        except Exception as e:  # noqa: BLE001
            notes.append(f"CSPROBE QMP failed: {e!r}")

    def failures(self, lines):
        out = []
        out.extend(self.results.get("csprobe", []))   # CSPROBE
        if self.error:
            out.append(f"shell harness: {self.error}")'''

B_ANCHOR = '''        from shelltest import ShellTest
        shelltest = ShellTest(burst=args.shell_burst)
'''
B_PROBE = '''        from shelltest import ShellTest
        shelltest = ShellTest(burst=args.shell_burst)
        if not env.get("QEMU_QMP"):   # CSPROBE: a QMP socket for the stall dump
            import tempfile
            env["QEMU_QMP"] = os.path.join(tempfile.mkdtemp(prefix="cosmo-csp-"), "qmp.sock")
        # Whatever socket QEMU is given -- the keyboard harness sets its own
        # when it runs, and that one wins -- is the one the dump must use.
        os.environ["QEMU_QMP"] = env["QEMU_QMP"]
'''


def cycles_prefix(n):
    out = ["COMMANDS = [\n", "    # --- CSPROBE (tools/console-stall-probe.py; not for merge) ---\n"]
    for i in range(n):
        out.append(f'    ("sleep 1", []), (INTERRUPT, []), ("echo csprobe-after-interrupt-{i:03d}-abcdefghijklmnopqrstuvwxyz", []),\n')
        # The background job exits one second after its start; the next
        # line goes out at 0.85-1.15 s, walking across that moment.
        pause = 0.85 + 0.3 * ((i * 7) % n) / max(n - 1, 1)
        out.append(f'    ("sleep 1 &", []), (PAUSE({pause:.3f}), []), ("echo csprobe-after-background-{i:03d}-abcdefghijklmnopqrstuvwxyz", []),\n')
    out.append("    # --- end CSPROBE ---\n")
    return "".join(out)


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def files(n):
    return [
        (SHELL, [(S_ANCHOR_CMDS, cycles_prefix(n)), (S_ANCHOR_FAIL, S_PROBE_FAIL),
                 (S_ANCHOR_FAILURES, S_PROBE_FAILURES)]),
        (BOOT, [(B_ANCHOR, B_PROBE)]),
    ]


def apply():
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 25
    if os.path.exists(STAMP):
        sys.exit('already applied')
    fl = files(n)
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
    print(f'applied ({n} cycles)')


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


def symbolize():
    # symbolize REGS ELF: every PC in the dump, with its function.
    regs, elf = sys.argv[2], sys.argv[3]
    text = open(regs).read()
    pcs = re.findall(r'\b(?:PC|RIP)\s*=\s*([0-9a-fA-F]+)', text)
    tool = shutil.which('llvm-symbolizer') or shutil.which('/opt/homebrew/opt/llvm/bin/llvm-symbolizer')
    cpu = 0
    for block in text.split('==== sample'):
        if not block.strip():
            continue
        print('---- sample')
        for i, pc in enumerate(re.findall(r'\b(?:PC|RIP)\s*=\s*([0-9a-fA-F]+)', block)):
            fn = '?'
            if tool:
                r = subprocess.run([tool, '--obj', elf, '--functions=short', '0x' + pc], capture_output=True, text=True)
                fn = ' '.join(r.stdout.split()[:2])
            print(f'  cpu {i}: pc {pc}  {fn}')
    del pcs, cpu


if __name__ == '__main__':
    {'apply': apply, 'revert': revert, 'symbolize': symbolize}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                                                     lambda: sys.exit(__doc__))()
