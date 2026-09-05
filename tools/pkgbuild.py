#!/usr/bin/env python3
"""pkgbuild.py - Build CosmoOS binary packages from recipes (docs/pkg/).

  pkgbuild.py build --port DIR --out REPO --cc CC --cflags "..." --ld LD --ldflags "..."
                    --ldscript user.ld --crt0 crt0.o --libc libc.a --sign-key KEY [--test-fixtures]
  pkgbuild.py index --repo REPO --sign-key KEY

A recipe (`port`) is `key: value` lines: name, version, summary, prefix,
depends (repeatable), program (destination then C sources; compiled with
the userland toolchain, mode 0755), file (destination, source, mode).
The package is a deterministic ustar archive (+MANIFEST first, then the
files sorted by path, mtime 0, uid/gid 0) followed by the COSMOSIG trailer
of scripts/modsign.py; INDEX describes the repository the same way.
Everything is a pure function of the tree, the flags and the key.
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts"))
import modsign  # noqa: E402

BLOCK = 512
NAME_RE = re.compile(r"^[a-z0-9][a-z0-9+._-]{0,62}$")
VERSION_RE = re.compile(r"^[0-9]+(\.[0-9]+)*(-[0-9]+)?$")
DEPEND_RE = re.compile(r"^([a-z0-9][a-z0-9+._-]*)\s*(?:(>=|<=|=|<|>)\s*([0-9]+(?:\.[0-9]+)*(?:-[0-9]+)?))?$")
FORBIDDEN_PREFIXES = ("boot/", "dev/", "tmp/", "mnt/")


def die(msg):
    raise SystemExit(f"pkgbuild: {msg}")


# --- recipes -----------------------------------------------------------------

def parse_recipe(path):
    rec = {"depends": [], "program": [], "file": [], "prefix": "/usr"}
    with open(path) as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            if ":" not in line:
                die(f"{path}:{lineno}: expected 'key: value'")
            key, value = line.split(":", 1)
            key, value = key.strip(), value.strip()
            if key in ("name", "version", "summary", "prefix"):
                rec[key] = value
            elif key == "depends":
                if not DEPEND_RE.match(value):
                    die(f"{path}:{lineno}: bad dependency '{value}'")
                rec["depends"].append(re.sub(r"\s+", " ", value))
            elif key == "program":
                parts = value.split()
                if len(parts) < 2:
                    die(f"{path}:{lineno}: program needs a destination and sources")
                rec["program"].append((parts[0], parts[1:]))
            elif key == "file":
                parts = value.split()
                if len(parts) not in (2, 3):
                    die(f"{path}:{lineno}: file needs 'destination source [mode]'")
                mode = int(parts[2], 8) if len(parts) == 3 else 0o644
                rec["file"].append((parts[0], parts[1], mode))
            else:
                die(f"{path}:{lineno}: unknown key '{key}'")
    for key in ("name", "version", "summary"):
        if key not in rec:
            die(f"{path}: missing '{key}'")
    if not NAME_RE.match(rec["name"]):
        die(f"{path}: bad name '{rec['name']}'")
    if not VERSION_RE.match(rec["version"]):
        die(f"{path}: bad version '{rec['version']}'")
    if not rec["prefix"].startswith("/"):
        die(f"{path}: prefix must be absolute")
    return rec


def dest_path(prefix, dest):
    if dest.startswith("/") or ".." in dest.split("/") or not dest:
        die(f"bad destination '{dest}'")
    full = os.path.normpath(os.path.join(prefix, dest)).lstrip("/")
    if any(full.startswith(p) for p in FORBIDDEN_PREFIXES) or len(full) > 100:
        die(f"destination not allowed: /{full}")
    return full


# --- archive -----------------------------------------------------------------

def octal(value, width):
    return ("%0*o" % (width - 1, value)).encode("ascii") + b"\0"


def tar_header(name, size, mode):
    name_b = name.encode("ascii")
    if len(name_b) > 100:
        die(f"name too long (>100 bytes): {name}")
    h = bytearray(BLOCK)
    h[0:len(name_b)] = name_b
    h[100:108] = octal(mode & 0o7777, 8)
    h[108:116] = octal(0, 8)
    h[116:124] = octal(0, 8)
    h[124:136] = octal(size, 12)
    h[136:148] = octal(0, 12)
    h[148:156] = b"        "
    h[156] = ord("0")
    h[257:263] = b"ustar\0"
    h[263:265] = b"00"
    chksum = sum(h)
    h[148:156] = ("%06o" % chksum).encode("ascii") + b"\0 "
    return bytes(h)


def tar_archive(members):
    """members: list of (name, mode, data) in the order to write."""
    chunks = []
    for name, mode, data in members:
        chunks.append(tar_header(name, len(data), mode))
        chunks.append(data)
        chunks.append(b"\0" * ((-len(data)) % BLOCK))
    chunks.append(b"\0" * (2 * BLOCK))
    return b"".join(chunks)


def sign(payload, key_path):
    seed = modsign.read_hex_key(key_path, "secret key")
    pub = modsign.secret_to_public(seed)
    sig = modsign.ed25519_sign(seed, payload)
    trailer = modsign.TRAILER.pack(sig, modsign.key_id(pub), modsign.VERSION, modsign.ALGO_ED25519, modsign.MAGIC)
    return payload + trailer


def write_atomic(path, blob):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(blob)
    os.replace(tmp, path)


# --- build -------------------------------------------------------------------

def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout)
        die(f"command failed: {' '.join(cmd)}")


def compile_program(args, port_dir, work, dest, sources):
    objs = []
    for src in sources:
        if src.startswith("/") or ".." in src.split("/"):
            die(f"source outside the port: {src}")
        spath = os.path.join(port_dir, src)
        if not os.path.isfile(spath):
            die(f"missing source {spath}")
        obj = os.path.join(work, src.replace("/", "_") + ".o")
        run([args.cc] + args.cflags.split() + ["-c", spath, "-o", obj])
        objs.append(obj)
    out = os.path.join(work, os.path.basename(dest) + ".elf")
    run([args.ld] + args.ldflags.split() + ["-T", args.ldscript, "-o", out, args.crt0] + objs + [args.libc])
    with open(out, "rb") as f:
        return f.read()


def manifest_text(rec, files):
    lines = [f"name: {rec['name']}", f"version: {rec['version']}", f"summary: {rec['summary']}"]
    lines += [f"depends: {d}" for d in rec["depends"]]
    for path, mode, data in files:
        lines.append(f"file: {path} {mode:04o} {len(data)} {hashlib.sha512(data).hexdigest()}")
    return ("\n".join(lines) + "\n").encode("ascii")


def cmd_build(args):
    port_dir = os.path.abspath(args.port)
    rec = parse_recipe(os.path.join(port_dir, "port"))
    work = os.path.join(args.out, ".build", f"{rec['name']}-{rec['version']}")
    os.makedirs(work, exist_ok=True)
    files = {}
    for dest, sources in rec["program"]:
        path = dest_path(rec["prefix"], dest)
        files[path] = (0o755, compile_program(args, port_dir, work, dest, sources))
    for dest, src, mode in rec["file"]:
        path = dest_path(rec["prefix"], dest)
        if src.startswith("/") or ".." in src.split("/"):
            die(f"source outside the port: {src}")
        with open(os.path.join(port_dir, src), "rb") as f:
            files[path] = (mode, f.read())
    ordered = [(p, files[p][0], files[p][1]) for p in sorted(files)]
    manifest = manifest_text(rec, ordered)
    payload = tar_archive([("+MANIFEST", 0o644, manifest)] + ordered)
    blob = sign(payload, args.sign_key)
    name = f"{rec['name']}-{rec['version']}.cpk"
    write_atomic(os.path.join(args.out, name), blob)
    print(f"  PKG      {name} ({len(ordered)} files, {len(blob)} bytes)")
    if args.test_fixtures and rec["name"] == "hello" and rec["version"] == "1.0":
        # badsig: a payload byte flipped after signing (the signature no longer matches).
        bad = bytearray(blob)
        bad[BLOCK + 5] ^= 0x01   # inside the manifest text: "name: hello" -> the signature check must catch it
        write_atomic(os.path.join(args.out, "badsig-1.0.cpk"), bytes(bad))
        # badsum: a valid signed package whose index checksum will be wrong (index records that).
        write_atomic(os.path.join(args.out, "badsum-1.0.cpk"), blob)
        print("  PKG      badsig-1.0.cpk badsum-1.0.cpk (test fixtures)")
    return 0


# --- index -------------------------------------------------------------------

def read_manifest_from_package(path):
    with open(path, "rb") as f:
        blob = f.read()
    payload, trailer = modsign.split_trailer(blob)
    if trailer is None:
        die(f"{path}: not signed")
    header = payload[:BLOCK]
    name = header[0:100].rstrip(b"\0").decode("ascii")
    size = int(header[124:135].rstrip(b"\0 ") or b"0", 8)
    if name != "+MANIFEST":
        die(f"{path}: first member is not +MANIFEST")
    manifest = payload[BLOCK:BLOCK + size].decode("ascii")
    fields = {"depends": []}
    for line in manifest.splitlines():
        key, _, value = line.partition(": ")
        if key == "depends":
            fields["depends"].append(value)
        elif key in ("name", "version", "summary"):
            fields[key] = value
    return fields, blob


def cmd_index(args):
    stanzas = []
    for fn in sorted(os.listdir(args.repo)):
        if not fn.endswith(".cpk"):
            continue
        fields, blob = read_manifest_from_package(os.path.join(args.repo, fn))
        sha = hashlib.sha512(blob).hexdigest()
        if fn.startswith("badsig-"):
            fields = {"name": "badsig", "version": "1.0", "summary": "test fixture: broken signature", "depends": []}
        if fn.startswith("badsum-"):
            fields = {"name": "badsum", "version": "1.0", "summary": "test fixture: wrong index checksum",
                      "depends": []}
            sha = "00" * 64
        lines = [f"name: {fields['name']}", f"version: {fields['version']}", f"summary: {fields['summary']}"]
        lines += [f"depends: {d}" for d in fields["depends"]]
        lines += [f"file: {fn}", f"size: {len(blob)}", f"sha512: {sha}"]
        stanzas.append((fields["name"], fields["version"], "\n".join(lines) + "\n"))
    stanzas.sort()
    text = "\n".join(s[2] for s in stanzas).encode("ascii")
    write_atomic(os.path.join(args.repo, "INDEX"), sign(text, args.sign_key))
    print(f"  INDEX    {len(stanzas)} packages")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--port", required=True)
    b.add_argument("--out", required=True)
    b.add_argument("--cc", required=True)
    b.add_argument("--cflags", default="")
    b.add_argument("--ld", required=True)
    b.add_argument("--ldflags", default="")
    b.add_argument("--ldscript", required=True)
    b.add_argument("--crt0", required=True)
    b.add_argument("--libc", required=True)
    b.add_argument("--sign-key", required=True)
    b.add_argument("--test-fixtures", action="store_true")
    b.set_defaults(fn=cmd_build)
    i = sub.add_parser("index")
    i.add_argument("--repo", required=True)
    i.add_argument("--sign-key", required=True)
    i.set_defaults(fn=cmd_index)
    args = ap.parse_args(argv[1:])
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
