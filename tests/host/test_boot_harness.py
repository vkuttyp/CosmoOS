#!/usr/bin/env python3
"""Host test of the boot harness's serial capture (tests/boot/install_test.py,
make host-test).

QEMU's own messages go to its stderr; the guest's serial line to its
stdout. A message QEMU prints in the middle of a guest line (main's
test-mirror, 2026-10-24: `wrong value for queue_enable 0` at a hot
unplug, inside `pool: ... MISSING`) must not split that line in what the
tests match. A fake qemu-run.sh writes half a line to stdout, a QEMU
message to stderr, then the rest of the line:
- Boot.text() holds the guest line whole, without QEMU's message;
- QEMU's message is kept, in the boot's -qemu.log beside its serial log.
"""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "boot"))
import install_test  # noqa: E402

FAKE = """#!/bin/sh
printf 'cosmoboot-uefi v1\\n'
printf 'pool: cosmo-root: member 0 copy 1 (vda2)'
sleep 0.3
printf 'qemu-system-x86_64: wrong value for queue_enable 0\\n' >&2
sleep 0.3
printf ' MISSING (-19)\\n\\ncosmo$ '
read line
exit 0
"""


def check(cond, what):
    if not cond:
        print(f"FAIL: {what}")
        sys.exit(1)
    print(f"ok: {what}")


def main():
    tmp = tempfile.mkdtemp(prefix="boot-harness-")
    try:
        os.makedirs(os.path.join(tmp, "scripts"))
        fake = os.path.join(tmp, "scripts", "qemu-run.sh")
        with open(fake, "w") as f:
            f.write(FAKE)
        os.chmod(fake, 0o755)
        install_test.ROOT = tmp
        b = install_test.Boot("fake", "unused.img", tmp, {}, 30)
        b.wait_prompt("the fake prompt")
        text = b.text()
        check("pool: cosmo-root: member 0 copy 1 (vda2) MISSING (-19)\n" in text,
              "a guest line QEMU wrote a message inside of is whole in the serial text")
        check("queue_enable" not in text, "QEMU's message is not in the serial text")
        b.kill()
        with open(os.path.join(tmp, "boot-fake-qemu.log")) as f:
            check("wrong value for queue_enable 0" in f.read(), "QEMU's message is kept in boot-fake-qemu.log")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("boot-harness: PASS")


if __name__ == "__main__":
    main()
