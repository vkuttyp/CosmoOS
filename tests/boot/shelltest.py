"""Host side of the interactive shell test (docs/userland/design.md).

QEMU's serial port is its stdin/stdout. When the serial log shows the
shell's prompt, this module types one command at a time and waits for
the next prompt, ending with `exit`, which ends init and the boot. The
tty echoes what is typed and the programs print their output, so the
checks are on the serial log.
"""
import re
import time

# A prompt at the start of a line. The shell draws its own line now, and
# an edit that moves text about redraws the whole thing -- prompt
# included -- after a carriage return. Counting bare "cosmo$ " would
# therefore count once per keystroke of an arrow-key edit and race the
# shell badly. A *real* prompt is the one that follows a newline: either
# the newline the shell echoes for Enter, or the end of a command's
# output.
PROMPT = b"\ncosmo$ "

# Sent as a raw byte in the middle of a running command rather than as a
# line: the shell is waiting for the job, not for a line, and the point
# is that the kernel turns the keystroke into a signal to the job's
# process group (docs/kernel/process/design.md, "Sessions and process
# groups").
INTERRUPT = "\x03"
SUSPEND = "\x1a"
INTERRUPT_DELAY_S = 0.5   # long enough for the job to be the foreground group
# What the interrupt has to prove is that the job died *early*: `sleep 5`
# reaches its prompt on its own eventually, so a run in which ^C did
# nothing at all still ends with a prompt and every pattern matched. The
# time from the keystroke to the next prompt is the only thing that
# separates the two, so it is measured and bounded.
INTERRUPT_MAX_S = 3.0

# (command, patterns the log must contain afterwards)
COMMANDS = [
    ("echo interactive-ok", [r"^interactive-ok$"]),
    ("ls /bin", [r"^sh$", r"^cat$"]),
    ("ps", [r"^\s*\d+\s+0\s+0\s+R\s+1\s+\d+\s+\d+\s+init$", r"\s+ps$"]),
    ("echo $((", [r"^interactive-ok$"]),   # a harmless odd line: nothing crashes
    ("pwd", [r"^/$"]),
    ("cd /tmp && pwd && cd /", [r"^/tmp$"]),
    ("sysctl kernel.name", [r"^kernel.name = CosmoOS$"]),
    ("dmesg", [r"\[ INFO\] tty: |\[ INFO\] serial: console input on IRQ \d+"]),
    ("nosuchprogram", [r"^sh: nosuchprogram: not found$"]),
    # ^C interrupts the job and not the shell: `sleep` dies, the terminal
    # echoes the keystroke, and the next prompt arrives without waiting
    # out the five seconds.
    ("sleep 5", []),
    (INTERRUPT, [r"\^C"]),
    ("echo after-interrupt-ok", [r"^after-interrupt-ok$"]),
    # ^Z stops the job instead of killing it: the shell says so, `jobs`
    # still lists it, and `fg` brings it back to be interrupted.
    ("sleep 30", []),
    (SUSPEND, [r"\[1\]\+  Stopped"]),
    ("jobs", [r"\[1\]\+  Stopped\s+sleep 30"]),
    ("fg", []),
    (INTERRUPT, []),
    ("echo after-fg-ok", [r"^after-fg-ok$"]),
    # A *pipeline* stopped by ^Z: every stage is in the job's group, so
    # the shell must wait for all of them to park before it takes the
    # terminal back, or a stage still running writes over the prompt.
    ("sleep 30 | cat", []),
    (SUSPEND, [r"\[\d+\]\+  Stopped\s+sleep 30 \| cat"]),
    ("jobs", [r"\[\d+\]\+  Stopped\s+sleep 30 \| cat"]),
    ("fg", []),
    (INTERRUPT, []),
    # Both stages must actually die: a shell that reported the job
    # stopped without waiting for every stage would hand `fg` a job it
    # then reports stopped again, and the ^C would reach nothing.
    # Line editing, which needs the terminal in raw mode: the shell draws
    # the line itself, so what runs is what is left after the edits. The
    # backspaces remove "XY" and the left arrows put "ok" before "-2".
    ("echo edit-okXY\x7f\x7f", [r"^edit-ok$"]),
    ("echo edit\x1b[D\x1b[D\x1b[D\x1b[D-2", [r"^-2edit$"]),
    # Escape that is not the start of a sequence: the byte after it is a
    # keystroke, not part of an escape the shell swallows. Without this
    # the `k` disappears and the line reads "esc-o".
    ("echo esc-o\x1bk", [r"^esc-ok$"]),
    # And a CSI sequence this shell does not know -- Delete is `Esc [ 3 ~`
    # -- is ignored whole. Reading only one byte after `[` left the `~`
    # behind and ran `echo del-ok~`.
    ("echo del-ok\x1b[3~", [r"^del-ok$"]),
    ("echo after-pipeline-ok", [r"^after-pipeline-ok$",
                                r"'sleep' exited with status 130",
                                r"'cat' exited with status 130"]),
    # And a background job: it runs without the terminal, and the shell
    # reports it finished before the next prompt.
    ("sleep 1 &", [r"^\[\d+\] \d+$"]),
    ("jobs", [r"\[\d+\][+ ]  (Running|Done)\s+sleep 1"]),
    ("pkg update && pkg install hello && hello && pkg list", [r"^hello, world \(hello 1\.1\)$", r"^hello\s+1\.1\s+prints a greeting$"]),
    ("exit 0", []),
]


class ShellTest:
    def __init__(self):
        self.results = {"prompts": 0, "sent": [], "interrupt_s": None}
        self.error = None

    def _wait_prompt(self, log_path, proc, deadline, count):
        """Wait until the log holds at least `count` prompts."""
        while time.monotonic() < deadline and proc.poll() is None:
            try:
                with open(log_path, "rb") as f:
                    data = f.read()
            except OSError:
                data = b""
            if data.count(PROMPT) >= count:
                return True
            time.sleep(0.1)
        return False

    def run(self, log_path, proc, timeout):
        deadline = time.monotonic() + timeout
        prompts = 0
        sent_at = None   # when the interrupt went out, until its prompt arrives
        try:
            for cmd, _ in COMMANDS:
                if cmd == SUSPEND:
                    # Like the interrupt: a raw byte at a running job,
                    # with no prompt to wait for first. The prompt it
                    # produces is counted by the next command's wait.
                    time.sleep(INTERRUPT_DELAY_S)
                    proc.stdin.write(SUSPEND.encode())
                    proc.stdin.flush()
                    sent_at = time.monotonic()
                    self.results["sent"].append(cmd)
                    continue
                if cmd == INTERRUPT:
                    # No prompt to wait for and no newline to send: the
                    # previous command is still running, which is the
                    # only state in which this means anything.
                    time.sleep(INTERRUPT_DELAY_S)
                    proc.stdin.write(INTERRUPT.encode())
                    proc.stdin.flush()
                    sent_at = time.monotonic()
                    self.results["sent"].append(cmd)
                    continue
                prompts += 1
                if not self._wait_prompt(log_path, proc, deadline, prompts):
                    self.error = f"no prompt before command {prompts} ({cmd!r})"
                    return
                if sent_at is not None:
                    self.results["interrupt_s"] = time.monotonic() - sent_at
                    sent_at = None
                self.results["prompts"] = prompts
                proc.stdin.write((cmd + "\n").encode())
                proc.stdin.flush()
                self.results["sent"].append(cmd)
                time.sleep(0.05)
        except Exception as e:  # noqa: BLE001
            self.error = repr(e)

    def failures(self, lines):
        out = []
        if self.error:
            out.append(f"shell harness: {self.error}")
        # The bound is on the *first* timed keystroke, which is the ^C
        # at `sleep 5`; the later ones share the counter and only make
        # it stricter, since each also has to beat its own job.
        took = self.results["interrupt_s"]
        if took is None:
            out.append("shell harness: no prompt was timed after the interrupt")
        elif took >= INTERRUPT_MAX_S:
            out.append(f"shell harness: the prompt took {took:.1f}s after ^C "
                       f"(>= {INTERRUPT_MAX_S}s: the job ran to completion, so ^C reached nothing)")
        for cmd, patterns in COMMANDS:
            if cmd not in self.results["sent"]:
                out.append(f"shell harness: never sent {cmd!r}")
                continue
            for pat in patterns:
                if not any(re.search(pat, ln) for ln in lines):
                    out.append(f"shell harness: after {cmd!r} missing /{pat}/")
        return out
