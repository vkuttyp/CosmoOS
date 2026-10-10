#!/usr/bin/env python3
"""mkgpt.py OUTPUT.img ESP.img

Wrap a FAT image in a GUID Partition Table: the boot image is a GPT disk
whose partition 1 is the EFI System Partition (docs/boot/design.md, "The
boot disk"). Firmware boots it as before; the loader reports the
partition it was read from (boot protocol v7), and the installer copies
that partition to the disk it installs (roadmap M2).

Layout (512-byte sectors): protective MBR, primary header, 128 entries of
128 bytes, partition 1 from LBA 2048 (1 MiB) for exactly the ESP image's
sectors, 1 MiB of slack, the backup entries and header on the last
sectors. The layout and checksums are the ones kernel/block/gpt.c builds
and parses.

The GUIDs are derived from the ESP image's contents, so a rebuild of the
same inputs gives the same disk byte for byte.
"""

import hashlib
import struct
import sys
import uuid
import zlib

SS = 512
ENTRIES = 128
ENTRY_SIZE = 128
ARRAY_SECTORS = ENTRIES * ENTRY_SIZE // SS
ESP_TYPE = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")
ALIGN = 2048


def derived_guid(seed, label):
    h = hashlib.sha256(label.encode() + seed).digest()[:16]
    u = uuid.UUID(bytes=h, version=4)
    return u.bytes_le


def header(my, alt, first, last, disk_guid, entries_lba, entries_crc):
    h = struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x00010000, 92, 0, 0, my, alt, first, last, disk_guid,
                    entries_lba, ENTRIES, ENTRY_SIZE, entries_crc)
    crc = zlib.crc32(h) & 0xFFFFFFFF
    h = h[:16] + struct.pack("<I", crc) + h[20:]
    return h + bytes(SS - len(h))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().splitlines()[0])
    out, esp_path = sys.argv[1], sys.argv[2]
    esp = open(esp_path, "rb").read()
    if len(esp) % SS:
        sys.exit("mkgpt: the ESP image is not a whole number of sectors")
    esp_sectors = len(esp) // SS
    first_usable = 2 + ARRAY_SECTORS
    p1_first = ALIGN
    p1_last = p1_first + esp_sectors - 1
    total = p1_last + 1 + ALIGN
    last_usable = total - ARRAY_SECTORS - 2
    seed = hashlib.sha256(esp).digest()
    disk_guid = derived_guid(seed, "disk")
    part_guid = derived_guid(seed, "esp")

    entry = ESP_TYPE.bytes_le + part_guid + struct.pack("<QQQ", p1_first, p1_last, 0)
    entry += "EFI system partition".encode("utf-16-le")
    entry += bytes(ENTRY_SIZE - len(entry))
    entries = entry + bytes(ENTRY_SIZE * (ENTRIES - 1))
    ecrc = zlib.crc32(entries) & 0xFFFFFFFF

    mbr = bytearray(SS)
    rec = struct.pack("<B3sB3sII", 0, b"\x00\x02\x00", 0xEE, b"\xff\xff\xff", 1, min(total - 1, 0xFFFFFFFF))
    mbr[446:446 + 16] = rec
    mbr[510:512] = b"\x55\xaa"

    with open(out + ".tmp", "wb") as f:
        f.write(mbr)
        f.write(header(1, total - 1, first_usable, last_usable, disk_guid, 2, ecrc))
        f.write(entries)
        f.seek(p1_first * SS)
        f.write(esp)
        f.seek((total - 1 - ARRAY_SECTORS) * SS)
        f.write(entries)
        f.write(header(total - 1, 1, first_usable, last_usable, disk_guid, total - 1 - ARRAY_SECTORS, ecrc))
    import os
    os.replace(out + ".tmp", out)


if __name__ == "__main__":
    main()
