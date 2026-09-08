"""Host side of the keyboard test (docs/drivers/usb/testing.md).

The guest cannot type at itself, so this module does: it opens QEMU's
monitor protocol socket and sends key events into the emulated USB
keyboard, which is as close to a person at a keyboard as a test gets --
the events go through the device model, the device reports them on its
interrupt endpoint, the driver translates them, and the guest's
`hid-keyboard` self-test reads the line back out of the console tty.

The guest prints HID-KEYTEST-READY when it is waiting, so the keys
cannot arrive while an earlier test still owns the tty. fw_cfg tells the
guest that anything will type at all (opt/cosmo/keytest); without it the
self-test skips instead of waiting.
"""
import json
import os
import socket
import tempfile
import time

READY = b"HID-KEYTEST-READY"

# What is typed, and what kernel/device/hidtest.c expects to read back.
LINE = "cosmo Types 42!"

# A second line, typed with the keys overlapping: x goes down, then y
# goes down while x is still held, then both come up. A boot report is
# the set of keys held, not a stream of events, so the driver must send a
# character only for a key that was not in the previous report -- with
# both keys down the report says "x and y", and a driver that reports
# what it sees rather than what changed would type "xxyy". Nobody types
# this way on purpose; everybody types this way in a hurry.
ROLLOVER = "xy"

# QEMU's key names (qapi/ui.json QKeyCode) for what LINE needs. Shifted
# characters are sent as shift + the unshifted key, which is what a
# keyboard does and what makes the driver's modifier handling matter.
UNSHIFTED = {
    " ": "spc", "-": "minus", "=": "equal", "[": "bracket_left", "]": "bracket_right",
    ";": "semicolon", "'": "apostrophe", "`": "grave_accent", ",": "comma", ".": "dot",
    "/": "slash", "\\": "backslash", "\n": "ret",
}
SHIFTED = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8",
    "(": "9", ")": "0", "_": "minus", "+": "equal", "{": "bracket_left", "}": "bracket_right",
    ":": "semicolon", '"': "apostrophe", "~": "grave_accent", "<": "comma", ">": "dot",
    "?": "slash", "|": "backslash",
}


def _key_events(ch):
    """(qkeycode, shift) for one character, or None if it cannot be typed."""
    if ch.isalpha() and ch.islower():
        return ch, False
    if ch.isalpha() and ch.isupper():
        return ch.lower(), True
    if ch.isdigit():
        return ch, False
    if ch in UNSHIFTED:
        return UNSHIFTED[ch], False
    if ch in SHIFTED:
        return SHIFTED[ch], True
    return None


class KeyTest:
    def __init__(self):
        self.sock_path = os.path.join(tempfile.mkdtemp(prefix="cosmo-qmp-"), "qmp.sock")
        self.results = {"typed": 0}
        self.error = None
        self.ran = False

    def env(self):
        return {"QEMU_QMP": self.sock_path, "QEMU_FWCFG_KEYTEST": "1"}

    def _connect(self, deadline):
        while time.monotonic() < deadline:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(5)
                s.connect(self.sock_path)
                return s
            except OSError:
                time.sleep(0.1)
        return None

    @staticmethod
    def _command(sock, cmd, arguments=None):
        msg = {"execute": cmd}
        if arguments is not None:
            msg["arguments"] = arguments
        sock.sendall((json.dumps(msg) + "\n").encode())
        # Read replies until one is not an asynchronous event.
        buf = b""
        while True:
            chunk = sock.recv(65536)
            if not chunk:
                raise OSError("QMP closed")
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if not line.strip():
                    continue
                reply = json.loads(line)
                if "event" in reply:
                    continue
                if "error" in reply:
                    raise OSError(f"QMP {cmd}: {reply['error']}")
                return reply

    def _send_rollover(self, sock):
        """x down, y down, x up, y up: two keys held at once."""
        def ev(down, qcode):
            return {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": qcode}}}

        for events in ([ev(True, "x")], [ev(True, "y")], [ev(False, "x")], [ev(False, "y")]):
            self._command(sock, "input-send-event", {"events": events})
            time.sleep(0.02)

    def _send_char(self, sock, ch):
        key = _key_events(ch)
        if key is None:
            raise OSError(f"cannot type {ch!r}")
        qcode, shift = key
        events = []
        if shift:
            events.append({"type": "key", "data": {"down": True,
                                                   "key": {"type": "qcode", "data": "shift"}}})
        events.append({"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": qcode}}})
        events.append({"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": qcode}}})
        if shift:
            events.append({"type": "key", "data": {"down": False,
                                                   "key": {"type": "qcode", "data": "shift"}}})
        self._command(sock, "input-send-event", {"events": events})

    def _wait_ready(self, log_path, proc, deadline):
        while time.monotonic() < deadline and proc.poll() is None:
            try:
                with open(log_path, "rb") as f:
                    if READY in f.read():
                        return True
            except OSError:
                pass
            time.sleep(0.1)
        return False

    def run_when_ready(self, log_path, proc, timeout):
        deadline = time.monotonic() + timeout
        try:
            if not self._wait_ready(log_path, proc, deadline):
                self.error = "the guest never asked for keys (no HID-KEYTEST-READY)"
                return
            sock = self._connect(deadline)
            if sock is None:
                self.error = f"no QMP socket at {self.sock_path}"
                return
            with sock:
                self._command(sock, "qmp_capabilities")
                for ch in LINE + "\n":
                    self._send_char(sock, ch)
                    self.results["typed"] += 1
                    time.sleep(0.02)   # one report per key, in order
                self._send_rollover(sock)
                self.results["typed"] += len(ROLLOVER)
                self._send_char(sock, "\n")
                self.results["typed"] += 1
            self.ran = True
        except Exception as e:  # noqa: BLE001
            self.error = repr(e)

    def failures(self):
        out = []
        if self.error:
            out.append(f"key harness: {self.error}")
        else:
            want = len(LINE) + 1 + len(ROLLOVER) + 1
            if self.results["typed"] != want:
                out.append(f"key harness: typed {self.results['typed']} of {want} characters")
        return out
