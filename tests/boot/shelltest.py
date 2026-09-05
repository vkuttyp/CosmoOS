"""Host side of the interactive shell test (docs/userland/design.md).

QEMU's serial port is its stdin/stdout. When the serial log shows the
shell's prompt, this module types one command at a time and waits for
the next prompt, ending with `exit`, which ends init and the boot. The
tty echoes what is typed and the programs print their output, so the
checks are on the serial log.
"""
import re
import time

PROMPT = b"cosmo$ "

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
    ("pkg update && pkg install hello && hello && pkg list", [r"^hello, world \(hello 1\.1\)$", r"^hello\s+1\.1\s+prints a greeting$"]),
    ("exit 0", []),
]


class ShellTest:
    def __init__(self):
        self.results = {"prompts": 0, "sent": []}
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
        try:
            for i, (cmd, _) in enumerate(COMMANDS):
                if not self._wait_prompt(log_path, proc, deadline, i + 1):
                    self.error = f"no prompt before command {i + 1} ({cmd!r})"
                    return
                self.results["prompts"] = i + 1
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
        for cmd, patterns in COMMANDS:
            if cmd not in self.results["sent"]:
                out.append(f"shell harness: never sent {cmd!r}")
                continue
            for pat in patterns:
                if not any(re.search(pat, ln) for ln in lines):
                    out.append(f"shell harness: after {cmd!r} missing /{pat}/")
        return out
