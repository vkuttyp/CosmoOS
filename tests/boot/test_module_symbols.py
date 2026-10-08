#!/usr/bin/env python3
"""The harness resolves module text (docs/development.md, the symbols table).

The loader prints `module: base <name> text <addr> size <n> ...` for each
module, and the harness resolves an address in that range against the
module's own object, laying out its text group as kernel/module/modelf.c
does: every allocatable executable section in section order, each at its
alignment. These checks build a two-text-section object with the host's
clang and hold the layout, the function names and (with llvm-symbolizer)
the source lines to what the object says.

    COSMO_CC=clang COSMO_SYMBOLIZER=llvm-symbolizer python3 tests/boot/test_module_symbols.py
"""

import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_boot_test import ko_text, symbolize  # noqa: E402

FAILURES = []
CHECKS = 0


def check(cond, what):
    global CHECKS
    CHECKS += 1
    print(f"{'ok  ' if cond else 'FAIL'} {what}")
    if not cond:
        FAILURES.append(what)


SOURCE = r"""
int g;
__attribute__((noinline)) int hot_one(int x) { g += x; return g * 3; }
__attribute__((noinline)) int hot_two(int x) { g -= x; return g ^ 5; }
__attribute__((noinline, section(".text.unlikely."))) int cold_one(int x) { g *= x; return g + 7; }
"""

cc = os.environ.get("COSMO_CC", "clang")
tool = os.environ.get("COSMO_SYMBOLIZER", "llvm-symbolizer")
work = tempfile.mkdtemp()
try:
    src = os.path.join(work, "mod.c")
    open(src, "w").write(SOURCE)
    mods = os.path.join(work, "modules")
    os.makedirs(mods)
    obj = os.path.join(mods, "testmod.ko.unsigned")
    r = subprocess.run([cc, "--target=x86_64-unknown-none-elf", "-O1", "-g", "-ffreestanding", "-fno-pic",
                        "-c", src, "-o", obj], capture_output=True, text=True)
    if r.returncode != 0:
        print("skip: no cross clang to build a module object (" + r.stderr.strip()[:120] + ")")
        sys.exit(0)

    texts, funcs = ko_text(obj)
    names = [n for _i, _o, _s, n in texts]
    check(names == [".text", ".text.unlikely."], f"two text sections, in section order (got {names})")
    check(texts[0][1] == 0, "the first at offset 0")
    first_end = texts[0][1] + texts[0][2]
    check(texts[1][1] >= first_end and texts[1][1] < first_end + 16,
          f"the second right after it, at its alignment (got 0x{texts[1][1]:x} after 0x{first_end:x})")
    byname = {n: (sh, v) for (sh, v, _s, n) in funcs}
    check(set(byname) >= {"hot_one", "hot_two", "cold_one"}, "the function symbols are read")

    base = 0xffffffff88014000
    def at(fn, plus=4):
        sh, v = byname[fn]
        off = next(o for i, o, _s, _n in texts if i == sh)
        return "0x%x" % (base + off + v + plus)

    # An earlier module unloaded from the same range: the latest load wins.
    lines = ["[ INFO] module: base oldmod text 0x%x size 0x1000 rodata 0x0 data 0x0" % base,
             "[ INFO] module: base testmod text 0x%x size 0x1000 rodata 0x0 data 0x0" % base,
             "  #0  " + at("hot_two"), "  #1  " + at("cold_one"), "  #2  0x%x" % (base + 0x1000 + 8)]
    kernel = os.path.join(work, "kernel.elf")
    shutil.copy(obj, kernel)   # any existing file: the kernel's frames are not under test
    table = {a: (f, loc) for a, f, loc in symbolize(lines, kernel, tool, mods)}
    check(table.get(at("hot_two"), ("",))[0] == "hot_two [testmod]",
          f"a .text address names its function and the latest module loaded there (got {table.get(at('hot_two'))})")
    check(table.get(at("cold_one"), ("",))[0] == "cold_one [testmod]",
          f"a second text section's address resolves by the loader's layout (got {table.get(at('cold_one'))})")
    check(not table.get("0x%x" % (base + 0x1000 + 8), ("",))[0].endswith("[testmod]"),
          "past the text range is not the module's")
    if shutil.which(tool) or os.path.exists(tool):
        check(table[at("hot_two")][1].endswith("mod.c:4"), f"its line, from the object's DWARF (got {table[at('hot_two')][1]})")
        check(table[at("cold_one")][1].endswith("mod.c:5"),
              f"the second section's line too, through the placed copy (got {table[at('cold_one')][1]})")
    else:
        print("skip: no llvm-symbolizer for the line checks")
finally:
    shutil.rmtree(work, ignore_errors=True)

print(f"test_module_symbols: {CHECKS - len(FAILURES)}/{CHECKS} checks passed")
sys.exit(1 if FAILURES else 0)
