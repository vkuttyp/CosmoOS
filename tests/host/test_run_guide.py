#!/usr/bin/env python3
"""Host test of tests/guide/run_guide.py's console checks (make host-test;
backlog TEST-19, docs/audit/2026-10-22-test19-guide-report.md).

With a fake session that answers each typed command from a script:
- a command answered `HTTP/1.1 503` is typed again and passes when a
  retry answers; one that keeps answering 503 is typed four times and
  fails; a 404 is not retried;
- in an `optional` block a command whose expected line is missing is
  noted and the block's next command is still typed and checked; outside
  one the same answer fails the run;
- `optional` is accepted on a console block's test line only.
"""
import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tests", "guide"))
import run_guide as g  # noqa: E402

g.RETRY_5XX = (0, 0, 0)
FAILS = 0


class Session:
    prompt = "cosmo$ "

    def __init__(self, answers):
        self.answers = dict(answers)   # command -> list of outputs, one per typing
        self.typed = []

    def type(self, cmd):
        self.typed.append(cmd)

    def wait_prompt(self, what):
        cmd = self.typed[-1]
        outs = self.answers[cmd]
        return cmd + "\n" + (outs.pop(0) if len(outs) > 1 else outs[0])


def ok(cond, what):
    global FAILS
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        FAILS += 1


def raises(fn, exc):
    try:
        fn()
    except exc:
        return True
    return False


E503 = "wget: server returned error: HTTP/1.1 503 Service Unavailable"
GOOD = "<h1>Example Domain</h1>"
BODY = ["cosmo$ wget http://a/", "...Example Domain...", "cosmo$ wget https://a/", "...Example Domain..."]

def runs(fn):
    """True if fn returns without raising."""
    try:
        fn()
    except Exception:
        return False
    return True


s = Session({"wget http://a/": [E503, GOOD], "wget https://a/": [GOOD]})
ok(runs(lambda: g.run_console(s, BODY, "t")) and s.typed == ["wget http://a/", "wget http://a/", "wget https://a/"], "a 503 is typed again and the retry passes")

s = Session({"wget http://a/": [E503], "wget https://a/": [GOOD]})
ok(raises(lambda: g.run_console(s, BODY, "t"), g.Fail) and s.typed.count("wget http://a/") == 4,
   "a command that keeps answering 503 fails after four tries")

s = Session({"wget http://a/": ["wget: server returned error: HTTP/1.1 404 Not Found"], "wget https://a/": [GOOD]})
ok(raises(lambda: g.run_console(s, BODY, "t"), g.Fail) and s.typed == ["wget http://a/"], "a 404 is not retried")

warned = []
s = Session({"wget http://a/": ["nothing like it"], "wget https://a/": [GOOD]})
ok(runs(lambda: g.run_console(s, BODY, "t", warned)) and s.typed == ["wget http://a/", "wget https://a/"] and warned == ["t: wget http://a/"],
   "optional: a missing line is noted and the next command still runs")

warned = []
s = Session({"wget http://a/": [GOOD], "wget https://a/": ["nothing like it"]})
ok(runs(lambda: g.run_console(s, BODY, "t", warned)) and warned == ["t: wget https://a/"], "optional: the second command's own check is made")

s = Session({"wget http://a/": ["nothing like it"], "wget https://a/": [GOOD]})
ok(raises(lambda: g.run_console(s, BODY, "t"), g.Fail), "outside an optional block the same answer fails")

with tempfile.TemporaryDirectory() as d:
    old = g.GUIDE
    g.GUIDE = d
    with open(os.path.join(d, "p.md"), "w") as f:
        f.write("<!-- test: console optional -->\n```console\ncosmo$ true\n```\n"
                "<!-- test: host optional -->\n```sh\ntrue\n```\n")
    ok(raises(lambda: g.parse("p.md"), g.Fail), "optional on a host block is refused")
    with open(os.path.join(d, "q.md"), "w") as f:
        f.write("<!-- test: console optional -->\n```console\ncosmo$ true\n```\n")
    ok(runs(lambda: g.parse("q.md")) and g.parse("q.md")[0][2].get("optional") is True,
       "optional on a console block is read")
    g.GUIDE = old

print(f"test_run_guide: {'FAIL' if FAILS else 'PASS'}")
sys.exit(1 if FAILS else 0)
