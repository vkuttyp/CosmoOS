#!/usr/bin/env python3
"""run_guide.py -- run every command in the user guide (docs/guide/) against
a release image, so the guide cannot drift from the system (roadmap M6;
docs/guide/README.md, "How this guide is tested").

    run_guide.py --arch x86_64|aarch64 --host linux|macos --release DIR
                 [--workdir DIR] [--pages a.md,b.md] [--timeout S]

The guide's pages run in the order docs/guide/README.md lists them, in one
working directory that starts with the release set (DIR: the files of
`make release`, cosmoos-ARCH.img and SHA256SUMS among them), as a reader's
download directory would. Every fenced block of the guide is preceded by
an HTML comment that says how it runs; a ```sh or ```console block without
one is an error, so no command can be left untested.

  <!-- test: host [linux|macos] [x86_64|aarch64] -->   ```sh
      Each command (a line, with backslash continuations) runs in one bash
      that persists for the whole run (directory, variables and background
      jobs carry over) and must exit 0.
  <!-- test: session [linux|macos] [x86_64|aarch64] prompt="cosmo$ " -->   ```sh
      One command that is interactive (QEMU on its serial console, ssh):
      started with its input and output connected to this script, which
      waits for the prompt (at the start of a line); a QEMU session whose
      firmware never hands over to the loader is started once more, as
      every harness boot is (tests/boot/firmware_stall.py). It becomes the
      current session; one started while another runs (ssh while QEMU
      runs) is current until it ends, and then the one under it again.
  <!-- test: console -->   ```console
      Typed into the current session. A line starting with the session's
      prompt is a command, typed when the prompt is shown; the lines after
      it are output that must appear, in that order, before the next
      prompt (other lines may come between them). In an expected line,
      `...` matches any text. A command followed by the single line
      `(no output)` must print nothing but the kernel's own log lines
      (`[ INFO] ...`). `exit` (or `poweroff`) ends the session: the process
      must exit 0, or, for a QEMU session, 33: the status CosmoOS's clean
      power-off gives QEMU on both machines (x86-64's debug-exit device
      and AArch64's semihosting exit, docs/development.md "Exit-code
      contract").
      A console block whose test line says `optional` checks a site the
      project does not control (example.com): a line it expected and did
      not get is printed as a warning and the run goes on. Its commands
      must still return to the prompt in time.

Any console command whose output says the server answered 5xx (BusyBox
wget's `server returned error: HTTP/1.x 5NN`) is typed again, up to three
times, after 5, 10 and 20 s: the guide fetches from public servers, and a
server's 503 is not the system's (docs/testing/flakes.md, TEST-19).

A block whose host or architecture does not match the run is skipped:
the guide gives x86-64 and AArch64, Linux and macOS variants of the steps
that differ. Exit 0 PASS, 1 FAIL; every session's transcript is kept in
the working directory.
"""

import argparse
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tests", "boot"))
import firmware_stall  # noqa: E402
GUIDE = os.path.join(ROOT, "docs", "guide")
MARK = re.compile(r"^<!--\s*test:\s*(.*?)\s*-->\s*$")
FENCE = re.compile(r"^```(\w*)\s*$")


class Fail(Exception):
    pass


class Expected(Fail):
    """An expected output line missing (the session itself is fine)."""


def pages_in_order():
    """The pages docs/guide/README.md links to, in the order it lists them."""
    text = open(os.path.join(GUIDE, "README.md"), encoding="utf-8").read()
    seen = []
    for m in re.finditer(r"\]\(([a-z0-9-]+\.md)\)", text):
        if m.group(1) not in seen:
            seen.append(m.group(1))
    return seen


def parse(page):
    """[(line, kind, attrs, lang, body_lines)] for every fenced block."""
    lines = open(os.path.join(GUIDE, page), encoding="utf-8").read().split("\n")
    blocks = []
    i = 0
    pending = None
    while i < len(lines):
        m = MARK.match(lines[i])
        if m:
            pending = (i + 1, m.group(1))
            i += 1
            continue
        f = FENCE.match(lines[i])
        if f:
            lang = f.group(1)
            j = i + 1
            while j < len(lines) and not lines[j].startswith("```"):
                j += 1
            body = lines[i + 1:j]
            if lang in ("sh", "console"):
                if pending is None or pending[0] != i:
                    raise Fail(f"{page}:{i + 1}: a ```{lang} block with no <!-- test: ... --> line before it")
                words = pending[1].split()
                kind, rest = words[0], " ".join(words[1:])
                attrs = {"prompt": None, "host": None, "arch": None, "optional": False}
                pm = re.search(r'prompt="([^"]*)"', rest)
                if pm:
                    attrs["prompt"] = pm.group(1)
                    rest = rest[:pm.start()] + rest[pm.end():]
                for w in rest.split():
                    if w in ("linux", "macos"):
                        attrs["host"] = w
                    elif w in ("x86_64", "aarch64"):
                        attrs["arch"] = w
                    elif w == "optional" and kind == "console":
                        attrs["optional"] = True
                    else:
                        raise Fail(f"{page}:{pending[0]}: unknown word {w!r} in the test line")
                if kind not in ("host", "session", "console"):
                    raise Fail(f"{page}:{pending[0]}: unknown test kind {kind!r}")
                if (kind == "console") != (lang == "console"):
                    raise Fail(f"{page}:{pending[0]}: a {kind} test needs a ```{'console' if kind == 'console' else 'sh'} block")
                if kind == "session" and not attrs["prompt"]:
                    raise Fail(f"{page}:{pending[0]}: a session needs prompt=\"...\"")
                blocks.append((i + 1, kind, attrs, lang, body))
            pending = None
            i = j + 1
            continue
        if lines[i].strip():
            pending = None if pending and pending[0] != i else pending
        i += 1
    return blocks


def commands(body):
    """Join backslash-continued lines into whole commands."""
    out, cur = [], ""
    for ln in body:
        if not ln.strip() and not cur:
            continue
        if ln.rstrip().endswith("\\"):
            cur += ln.rstrip()[:-1] + " "
        else:
            out.append((cur + ln).strip())
            cur = ""
    if cur:
        out.append(cur.strip())
    return [c for c in out if c and not c.startswith("#")]


class HostShell:
    """One bash for every host command of the run."""

    def __init__(self, workdir, log):
        self.log = log
        self.proc = subprocess.Popen(["bash", "--noprofile", "--norc"], cwd=workdir, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.n = 0

    def run(self, cmd, timeout):
        self.n += 1
        tag = f"__guide_done_{self.n}__"
        self.log.write(f"$ {cmd}\n")
        # Its input is /dev/null, not this script's pipe: a command must
        # not read the lines that follow it (ssh would).
        # The marker on a line of its own even when the output did not end
        # with a newline.
        self.proc.stdin.write(f"{{\n{cmd}\n}} </dev/null\n__rc=$?; echo; echo \"{tag} $__rc\"\n")
        self.proc.stdin.flush()
        out = []
        result = {}

        def reader():
            for line in self.proc.stdout:
                if line.startswith(tag + " "):
                    result["rc"] = int(line.split()[1])
                    return
                out.append(line)
                self.log.write(line)

        t = threading.Thread(target=reader, daemon=True)
        t.start()
        t.join(timeout)
        if t.is_alive():
            raise Fail(f"host command did not finish in {timeout:.0f}s: {cmd}")
        if result.get("rc") != 0:
            raise Fail(f"host command exited {result.get('rc')}: {cmd}\n" + "".join(out[-20:]))
        return "".join(out)

    def close(self):
        try:
            self.proc.stdin.write("kill $(jobs -p) 2>/dev/null; exit 0\n")
            self.proc.stdin.flush()
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()


class Session:
    """An interactive command: QEMU on its serial console, or ssh."""

    def __init__(self, cmd, prompt, workdir, log_path, timeout):
        self.prompt = prompt
        self.timeout = timeout
        self.lock = threading.Lock()
        self.consumed = 0          # bytes of output already matched
        self.cmd = cmd
        qemu = cmd.split()[0].startswith("qemu-system-")
        # 33 is a clean power-off only from QEMU (both machines: the
        # debug-exit device and the semihosting exit report the kernel's
        # success the same way); any other session, ssh, must end 0.
        self.ok_status = (0, 33) if qemu else (0,)

        def launch(attempt):
            if attempt:
                os.replace(log_path, os.path.splitext(log_path)[0] + "-stall.log")
            self.buf = bytearray()
            self.log = open(log_path, "wb")
            self.proc = subprocess.Popen(["bash", "--noprofile", "--norc", "-c", "exec " + cmd], cwd=workdir,
                                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                         start_new_session=True)
            self.reader = threading.Thread(target=self._read, args=(self.proc, self.log, self.buf), daemon=True)
            self.reader.start()
            return self.proc

        if not qemu:
            launch(0)
            return

        # QEMU: the firmware-handover stall that every harness boot
        # retries once (tests/boot/firmware_stall.py; docs/testing/flakes.md,
        # "firmware handover"). Once the loader's banner is out, nothing is
        # retried.
        def read_log(attempt):
            with self.lock:
                return bytes(self.buf)

        def kill(proc):
            os.killpg(proc.pid, signal.SIGKILL)   # and anything it started
            proc.wait()
            self.reader.join()
            self.log.close()

        def on_retry():
            print(f"run-guide: {firmware_stall.RETRIED_NOTE} (no loader banner within "
                  f"{firmware_stall.HANDOVER_S:.0f}s)", flush=True)

        try:
            firmware_stall.launch_with_retry(launch, read_log, kill, on_retry)
        except firmware_stall.FirmwareStall as e:
            raise Fail(f"session '{cmd[:40]}': {e}")

    def _read(self, proc, log, buf):
        while True:
            b = proc.stdout.read1(4096)
            if not b:
                break
            with self.lock:
                buf += b
            log.write(b)
            log.flush()

    def text(self):
        with self.lock:
            return self.buf.decode("utf-8", "replace").replace("\r", "")

    def wait_prompt(self, what):
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            t = self.text()
            m = re.compile(r"(?:^|\n)" + re.escape(self.prompt)).search(t, self.consumed)
            if m:
                out = t[self.consumed:m.start()]
                self.consumed = m.end()
                return out
            if self.proc.poll() is not None:
                raise Fail(f"session '{self.cmd[:40]}' exited ({self.proc.returncode}) waiting for the prompt after {what}")
            time.sleep(0.1)
        raise Fail(f"no prompt {self.prompt!r} within {self.timeout:.0f}s after {what}")

    def type(self, cmd):
        self.proc.stdin.write(cmd.encode() + b"\n")
        self.proc.stdin.flush()

    def wait_exit(self, what):
        try:
            rc = self.proc.wait(timeout=self.timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            raise Fail(f"session did not end within {self.timeout:.0f}s after {what}")
        time.sleep(0.5)
        self.log.close()
        if rc not in self.ok_status:
            raise Fail(f"session ended with status {rc} after {what}")
        return self.text()[self.consumed:]

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()
        try:
            self.log.close()
        except Exception:
            pass


KLOG = re.compile(r"^\[ ?(DEBUG|INFO|WARN|ERROR)\] ")
SERVER_5XX = re.compile(r"server returned error: HTTP/1\.[01] 5\d\d")
RETRY_5XX = (5, 10, 20)   # seconds before each of the three retries


def expect(output, wanted, where):
    """Each wanted line must match a line of output, in order; `(no output)`
    alone means nothing but the echoed command and kernel log lines."""
    lines = [ln.rstrip() for ln in output.split("\n")]
    if wanted == ["(no output)"]:
        rest = [ln for ln in lines[1:] if ln.strip() and not KLOG.match(ln)]
        if rest:
            raise Expected(f"{where}: expected no output; got:\n" + "\n".join(rest[:20]))
        return
    k = 0
    for w in wanted:
        pat = "^" + ".*".join(re.escape(p) for p in w.rstrip().split("...")) + "$"
        while k < len(lines) and not re.match(pat, lines[k]):
            k += 1
        if k == len(lines):
            raise Expected(f"{where}: expected a line {w!r}; got:\n" + "\n".join(lines[-30:]))
        k += 1


def run_console(sess, body, where):
    """Type the commands of a console block, checking what each prints."""
    if sess is None:
        raise Fail(f"{where}: a console block with no session running")
    steps = []
    for ln in body:
        if ln.startswith(sess.prompt):
            steps.append((ln[len(sess.prompt):], []))
        elif steps:
            if ln.strip():
                steps[-1][1].append(ln)
        elif ln.strip():
            raise Fail(f"{where}: output before the first command: {ln!r}")
    for cmd, wanted in steps:
        sess.type(cmd)
        if cmd.strip() in ("exit", "poweroff"):
            out = sess.wait_exit(repr(cmd))
            expect(out, wanted, f"{where}: {cmd}")
            return True
        out = sess.wait_prompt(repr(cmd))
        for delay in RETRY_5XX:
            m = SERVER_5XX.search(out)
            if not m:
                break
            print(f"run-guide: {where}: {cmd}: {m.group(0)}; again in {delay} s", flush=True)
            time.sleep(delay)
            sess.type(cmd)
            out = sess.wait_prompt(repr(cmd))
        expect(out, wanted, f"{where}: {cmd}")
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arch", required=True, choices=["x86_64", "aarch64"])
    ap.add_argument("--host", required=True, choices=["linux", "macos"])
    ap.add_argument("--release", required=True, help="the release set (make release's out/release/ARCH)")
    ap.add_argument("--workdir")
    ap.add_argument("--pages", help="comma-separated subset, in guide order")
    ap.add_argument("--timeout", type=float, default=300.0, help="per command or prompt")
    args = ap.parse_args()
    if not (math.isfinite(args.timeout) and args.timeout > 0):
        ap.error("--timeout must be a positive number of seconds")

    # The working directory is this script's: made fresh, and an existing
    # one is removed only if an earlier run made it (its marker file), so a
    # mistyped --workdir deletes nothing.
    work = args.workdir or tempfile.mkdtemp(prefix="guide-")
    marker = os.path.join(work, ".run_guide")
    if os.path.isdir(work) and os.listdir(work):
        if not os.path.exists(marker):
            ap.error(f"--workdir {work} exists and was not made by run_guide.py; refusing to empty it")
        shutil.rmtree(work)
    os.makedirs(work, exist_ok=True)
    open(marker, "w").close()
    for f in os.listdir(args.release):
        shutil.copyfile(os.path.join(args.release, f), os.path.join(work, f))
    pages = pages_in_order()
    if args.pages:
        want = args.pages.split(",")
        unknown = [p for p in want if p not in pages]
        if unknown:
            ap.error(f"--pages: {', '.join(unknown)} not in docs/guide/README.md's list")
        pages = [p for p in pages if p in want]
    log = open(os.path.join(work, "host.log"), "w")
    shell = HostShell(work, log)
    stack = []   # running sessions, the current one last
    nsess = 0
    counts = {"host": 0, "session": 0, "console": 0, "skipped": 0}
    warned = []
    try:
        for page in pages:
            for line, kind, attrs, lang, body in parse(page):
                where = f"{page}:{line}"
                if (attrs["host"] and attrs["host"] != args.host) or (attrs["arch"] and attrs["arch"] != args.arch):
                    counts["skipped"] += 1
                    continue
                counts[kind] += 1
                print(f"run-guide: {where} {kind}", flush=True)
                if kind == "host":
                    for cmd in commands(body):
                        shell.run(cmd, args.timeout)
                elif kind == "session":
                    cmds = commands(body)
                    if len(cmds) != 1:
                        raise Fail(f"{where}: a session block holds exactly one command")
                    nsess += 1
                    stack.append(Session(cmds[0], attrs["prompt"], work,
                                         os.path.join(work, f"session-{nsess}.log"), args.timeout))
                    stack[-1].wait_prompt("the session's start")
                elif attrs["optional"]:
                    try:
                        if run_console(stack[-1] if stack else None, body, where):
                            stack.pop()
                    except Expected as e:
                        print(f"run-guide: WARN (optional block): {e}", flush=True)
                        warned.append(where)
                else:
                    if run_console(stack[-1] if stack else None, body, where):
                        stack.pop()
            if stack:
                raise Fail(f"{page}: ends with a session still running (end it with exit)")
    except Fail as e:
        for sess in stack:
            sess.kill()
        shell.close()
        print(f"run-guide: FAIL: {e}\nrun-guide: logs in {work}")
        return 1
    shell.close()
    print(f"run-guide: PASS ({args.host}, {args.arch}): {counts['host']} host blocks, {counts['session']} sessions, "
          f"{counts['console']} console blocks run, {counts['skipped']} for another host or architecture skipped; "
          + (f"optional blocks that warned: {', '.join(warned)}; " if warned else "") +
          f"logs in {work}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
