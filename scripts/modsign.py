#!/usr/bin/env python3
"""modsign.py - Sign and verify CosmoOS kernel modules (Ed25519).

  modsign.py keygen --out-key K.key --out-pub K.pub
  modsign.py sign   --key K.key --in module.ko.unsigned --out module.ko
  modsign.py verify --pub K.pub --in module.ko
  modsign.py keyid  --pub K.pub

Trailer format (kernel/include/kernel/modsig.h): sig[64] key_id[8]
version:u32 algo:u32 magic[8]="COSMOSIG", appended to the ELF. The
signature covers every byte before the trailer. Ed25519 is implemented
here in pure Python straight from RFC 8032 so the build needs nothing
beyond the standard library; signing a module takes well under a second.
Key files hold the 32-byte seed / public key as 64 hex characters.
"""

import argparse
import hashlib
import os
import secrets
import struct
import sys

# --- Ed25519 (RFC 8032 section 6 reference, unchanged in substance) ---

p = 2**255 - 19
q = 2**252 + 27742317777372353535851937790883648493


def modp_inv(x):
    return pow(x, p - 2, p)


d = -121665 * modp_inv(121666) % p
modp_sqrt_m1 = pow(2, (p - 1) // 4, p)


def sha512(s):
    return hashlib.sha512(s).digest()


def sha512_modq(s):
    return int.from_bytes(sha512(s), "little") % q


def point_add(P, Q):
    A = (P[1] - P[0]) * (Q[1] - Q[0]) % p
    B = (P[1] + P[0]) * (Q[1] + Q[0]) % p
    C = 2 * P[3] * Q[3] * d % p
    D = 2 * P[2] * Q[2] % p
    E, F, G, H = B - A, D - C, D + C, B + A
    return (E * F % p, G * H % p, F * G % p, E * H % p)


def point_mul(s, P):
    Q = (0, 1, 1, 0)
    while s > 0:
        if s & 1:
            Q = point_add(Q, P)
        P = point_add(P, P)
        s >>= 1
    return Q


def point_equal(P, Q):
    if (P[0] * Q[2] - Q[0] * P[2]) % p != 0:
        return False
    if (P[1] * Q[2] - Q[1] * P[2]) % p != 0:
        return False
    return True


def recover_x(y, sign):
    if y >= p:
        return None
    x2 = (y * y - 1) * modp_inv(d * y * y + 1)
    if x2 == 0:
        if sign:
            return None
        return 0
    x = pow(x2, (p + 3) // 8, p)
    if (x * x - x2) % p != 0:
        x = x * modp_sqrt_m1 % p
    if (x * x - x2) % p != 0:
        return None
    if (x & 1) != sign:
        x = p - x
    return x


g_y = 4 * modp_inv(5) % p
g_x = recover_x(g_y, 0)
G = (g_x, g_y, 1, g_x * g_y % p)


def point_compress(P):
    zinv = modp_inv(P[2])
    x = P[0] * zinv % p
    y = P[1] * zinv % p
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def point_decompress(s):
    if len(s) != 32:
        return None
    y = int.from_bytes(s, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % p)


def secret_expand(secret):
    if len(secret) != 32:
        raise ValueError("bad secret length")
    h = sha512(secret)
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return (a, h[32:])


def secret_to_public(secret):
    a, _ = secret_expand(secret)
    return point_compress(point_mul(a, G))


def ed25519_sign(secret, msg):
    a, prefix = secret_expand(secret)
    A = point_compress(point_mul(a, G))
    r = sha512_modq(prefix + msg)
    R = point_mul(r, G)
    Rs = point_compress(R)
    h = sha512_modq(Rs + A + msg)
    s = (r + h * a) % q
    return Rs + int.to_bytes(s, 32, "little")


def ed25519_verify(public, msg, signature):
    if len(public) != 32 or len(signature) != 64:
        return False
    A = point_decompress(public)
    if A is None:
        return False
    Rs = signature[:32]
    R = point_decompress(Rs)
    if R is None:
        return False
    s = int.from_bytes(signature[32:], "little")
    if s >= q:
        return False
    h = sha512_modq(Rs + public + msg)
    sB = point_mul(s, G)
    hA = point_mul(h, A)
    return point_equal(sB, point_add(R, hA))


# --- trailer ---

MAGIC = b"COSMOSIG"
VERSION = 1
ALGO_ED25519 = 1
TRAILER = struct.Struct("<64s8sII8s")
assert TRAILER.size == 88


def key_id(pub):
    return sha512(pub)[:8]


def read_hex_key(path, what):
    with open(path) as f:
        data = bytes.fromhex(f.read().strip())
    if len(data) != 32:
        raise SystemExit(f"modsign: {what} {path} is not 32 bytes")
    return data


def cmd_keygen(args):
    seed = secrets.token_bytes(32)
    pub = secret_to_public(seed)
    for path, blob in ((args.out_key, seed), (args.out_pub, pub)):
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, "w") as f:
            f.write(blob.hex() + "\n")
    os.chmod(args.out_key, 0o600)
    print(f"modsign: key id {key_id(pub).hex()} written to {args.out_key} / {args.out_pub}")
    return 0


def split_trailer(blob):
    if len(blob) < TRAILER.size or blob[-8:] != MAGIC:
        return blob, None
    sig, kid, version, algo, _ = TRAILER.unpack(blob[-TRAILER.size:])
    return blob[:-TRAILER.size], (sig, kid, version, algo)


def cmd_sign(args):
    seed = read_hex_key(args.key, "secret key")
    pub = secret_to_public(seed)
    with open(args.infile, "rb") as f:
        payload = f.read()
    payload, existing = split_trailer(payload)
    if existing is not None:
        sys.stderr.write("modsign: input already signed; re-signing the payload\n")
    sig = ed25519_sign(seed, payload)
    assert ed25519_verify(pub, payload, sig)
    trailer = TRAILER.pack(sig, key_id(pub), VERSION, ALGO_ED25519, MAGIC)
    tmp = args.out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(payload + trailer)
    os.replace(tmp, args.out)
    return 0


def cmd_verify(args):
    pub = read_hex_key(args.pub, "public key")
    with open(args.infile, "rb") as f:
        blob = f.read()
    payload, trailer = split_trailer(blob)
    if trailer is None:
        print("modsign: no signature trailer")
        return 1
    sig, kid, version, algo = trailer
    if version != VERSION or algo != ALGO_ED25519:
        print(f"modsign: unsupported trailer version {version} / algo {algo}")
        return 1
    if kid != key_id(pub):
        print(f"modsign: key id {kid.hex()} does not match {key_id(pub).hex()}")
        return 1
    if not ed25519_verify(pub, payload, sig):
        print("modsign: signature does not verify")
        return 1
    print(f"modsign: OK ({len(payload)} bytes, key {kid.hex()})")
    return 0


def cmd_keyid(args):
    pub = read_hex_key(args.pub, "public key")
    print(key_id(pub).hex())
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("keygen")
    k.add_argument("--out-key", required=True)
    k.add_argument("--out-pub", required=True)
    k.set_defaults(fn=cmd_keygen)
    s = sub.add_parser("sign")
    s.add_argument("--key", required=True)
    s.add_argument("--in", dest="infile", required=True)
    s.add_argument("--out", required=True)
    s.set_defaults(fn=cmd_sign)
    v = sub.add_parser("verify")
    v.add_argument("--pub", required=True)
    v.add_argument("--in", dest="infile", required=True)
    v.set_defaults(fn=cmd_verify)
    i = sub.add_parser("keyid")
    i.add_argument("--pub", required=True)
    i.set_defaults(fn=cmd_keyid)
    args = ap.parse_args(argv[1:])
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
