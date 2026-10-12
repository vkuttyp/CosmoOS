#!/usr/bin/env python3
"""mkmanifest.py - The signed boot manifest, \\cosmo\\manifest (roadmap M5).

  mkmanifest.py sign   --key K.key --out manifest KERNEL ARCHIVE CMDLINE
  mkmanifest.py verify --pub K.pub manifest KERNEL ARCHIVE CMDLINE
  mkmanifest.py resign --key K.key IMAGE

Layout (boot/protocol/cosmomanifest.h): magic "COSMOMAN", version 1, three
files -- kernel.elf, boot.tar, cmdline, in that order -- each a 16-byte NUL
padded name, a u64 size and the SHA-512 of the contents; then the module
signing trailer (scripts/modsign.py) over those 280 bytes. The UEFI loader
refuses to boot unless the signature is by a key in its ring and every file
matches (docs/boot/design.md, "Verified boot").

`resign` rewrites the manifest inside a boot disk image (scripts/mkimage.sh)
from the three files the image holds: what a test does after changing the
command line on purpose. The functions esp_get/esp_put below are the tests'
way into the image's ESP, through mtools at partition 1's offset.
"""

import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import modsign  # noqa: E402

MAGIC = b"COSMOMAN"
VERSION = 1
NAMES = (b"kernel.elf", b"boot.tar", b"cmdline")
ESP_PATHS = ("::/cosmo/kernel.elf", "::/cosmo/boot.tar", "::/cosmo/cmdline")
MANIFEST_PATH = "::/cosmo/manifest"
FILE = struct.Struct("<16sQ64s")
HEADER = struct.Struct("<8sII")
PAYLOAD_SIZE = HEADER.size + 3 * FILE.size
assert PAYLOAD_SIZE == 280


def payload(blobs):
    out = HEADER.pack(MAGIC, VERSION, len(NAMES))
    for name, blob in zip(NAMES, blobs):
        out += FILE.pack(name, len(blob), hashlib.sha512(blob).digest())
    return out


def sign(seed, blobs):
    body = payload(blobs)
    pub = modsign.secret_to_public(seed)
    sig = modsign.ed25519_sign(seed, body)
    assert modsign.ed25519_verify(pub, body, sig)
    return body + modsign.TRAILER.pack(sig, modsign.key_id(pub), modsign.VERSION, modsign.ALGO_ED25519,
                                       modsign.MAGIC)


def check(pub, manifest, blobs):
    """None when `manifest` is a valid signature by `pub` over `blobs`, else why not."""
    body, trailer = modsign.split_trailer(manifest)
    if trailer is None or len(body) != PAYLOAD_SIZE:
        return "not a signed manifest"
    sig, kid, version, algo = trailer
    if version != modsign.VERSION or algo != modsign.ALGO_ED25519 or kid != modsign.key_id(pub):
        return f"signed by key {kid.hex()}, not {modsign.key_id(pub).hex()}"
    if not modsign.ed25519_verify(pub, body, sig):
        return "the signature does not verify"
    if body != payload(blobs):
        return "the files do not match"
    return None


def read(path):
    with open(path, "rb") as f:
        return f.read()


def write_atomic(path, data):
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


# --- the ESP inside a boot disk image (mkimage.sh: GPT, partition 1) -------

def esp_offset(image):
    """Byte offset of partition 1 from the image's primary GPT."""
    with open(image, "rb") as f:
        f.seek(512)
        hdr = f.read(92)
        if hdr[:8] != b"EFI PART":
            raise SystemExit(f"mkmanifest: {image} has no GPT")
        entries_lba = struct.unpack_from("<Q", hdr, 72)[0]
        f.seek(entries_lba * 512)
        entry = f.read(128)
    return struct.unpack_from("<Q", entry, 32)[0] * 512


def esp_get(image, esp_path):
    off = esp_offset(image)
    return subprocess.run(["mcopy", "-n", "-i", f"{image}@@{off}", esp_path, "-"], check=True,
                          stdout=subprocess.PIPE).stdout


def esp_put(image, esp_path, data):
    off = esp_offset(image)
    with tempfile.NamedTemporaryFile(delete=False) as t:
        t.write(data)
    try:
        subprocess.run(["mcopy", "-o", "-n", "-i", f"{image}@@{off}", t.name, esp_path], check=True)
    finally:
        os.unlink(t.name)


def esp_delete(image, esp_path):
    subprocess.run(["mdel", "-i", f"{image}@@{esp_offset(image)}", esp_path], check=True)


def resign_image(image, seed):
    blobs = [esp_get(image, p) for p in ESP_PATHS]
    esp_put(image, MANIFEST_PATH, sign(seed, blobs))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("sign")
    s.add_argument("--key", required=True)
    s.add_argument("--out", required=True)
    s.add_argument("files", nargs=3)
    v = sub.add_parser("verify")
    v.add_argument("--pub", required=True)
    v.add_argument("manifest")
    v.add_argument("files", nargs=3)
    r = sub.add_parser("resign")
    r.add_argument("--key", required=True)
    r.add_argument("image")
    args = ap.parse_args(argv[1:])
    if args.cmd == "sign":
        seed = modsign.read_hex_key(args.key, "secret key")
        write_atomic(args.out, sign(seed, [read(p) for p in args.files]))
        return 0
    if args.cmd == "verify":
        why = check(modsign.read_hex_key(args.pub, "public key"), read(args.manifest), [read(p) for p in args.files])
        print(f"mkmanifest: {why or 'OK'}")
        return 1 if why else 0
    resign_image(args.image, modsign.read_hex_key(args.key, "secret key"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
