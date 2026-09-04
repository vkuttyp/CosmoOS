#!/usr/bin/env python3
"""check-module-elf.py module.ko

Post-build checks on a signed kernel module, independent of the kernel's
own validator so a regression in one is caught by the other:
  - ET_REL, x86-64, little endian ELF64
  - no section is both writable and executable (W^X, constitution s.15)
  - a .cosmo.module section of exactly 240 bytes exists
  - the file ends with a signature trailer (magic COSMOSIG)
"""

import struct
import sys

SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 1, 2, 4


def main(argv):
    if len(argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    path = argv[1]
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) < 88 or blob[-8:] != b"COSMOSIG":
        return fail(path, "missing signature trailer")
    elf = blob[:-88]
    if elf[:4] != b"\x7fELF" or elf[4] != 2 or elf[5] != 1:
        return fail(path, "not a little-endian ELF64")
    e_type, e_machine = struct.unpack_from("<HH", elf, 16)
    if e_type != 1:
        return fail(path, f"e_type {e_type} is not ET_REL")
    if e_machine != 62:
        return fail(path, f"e_machine {e_machine} is not x86-64")
    e_shoff, = struct.unpack_from("<Q", elf, 40)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", elf, 58)
    if e_shentsize != 64 or e_shoff + e_shnum * 64 > len(elf):
        return fail(path, "section header table out of bounds")
    shdrs = [struct.unpack_from("<IIQQQQIIQQ", elf, e_shoff + i * 64) for i in range(e_shnum)]
    strtab = shdrs[e_shstrndx]
    names = elf[strtab[4]:strtab[4] + strtab[5]]

    def name_of(sh):
        end = names.find(b"\0", sh[0])
        return names[sh[0]:end].decode("ascii", "replace")

    info_seen = False
    for sh in shdrs:
        flags = sh[2]
        if (flags & SHF_ALLOC) and (flags & SHF_WRITE) and (flags & SHF_EXECINSTR):
            return fail(path, f"section {name_of(sh)} is writable and executable")
        if name_of(sh) == ".cosmo.module":
            info_seen = True
            if sh[5] != 240:
                return fail(path, f".cosmo.module is {sh[5]} bytes, expected 240")
            if flags & SHF_WRITE:
                return fail(path, ".cosmo.module is writable")
    if not info_seen:
        return fail(path, "no .cosmo.module section")
    return 0


def fail(path, msg):
    sys.stderr.write(f"check-module-elf: {path}: {msg}\n")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
