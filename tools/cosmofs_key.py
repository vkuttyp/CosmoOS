#!/usr/bin/env python3
"""An encrypted cosmofs pool's keys on the host (roadmap 1.2 B5), for the
tests that must forge its metadata *correctly* -- tools/cosmofs-damage.py
damaging an encrypted root for test-encrypt's run of test-fsck.

    cosmofs_key.py --self-test

Pure Python, and slow: Argon2id here is for a slot made cheap on purpose
(`cosmo-key add --memory 8 --passes 1`), never for one made to resist a
guess. What it computes is the kernel's and cosmo-key's:

  - Argon2id (RFC 9106, version 0x13) on hashlib's BLAKE2b;
  - ChaCha20-Poly1305 (RFC 8439) to open a slot, with the nonce and the
    associated data cosmofs_format.h gives a slot (cfs_keyslot_nonce,
    cfs_keyslot_aad);
  - the MAC key: BLAKE2b-256 keyed by the master key over "cosmofs
    metadata MAC v1" (cfs_mac_derive), and a tag: BLAKE2b-64 keyed by it
    over the domain byte and the block with the tag's 8 bytes zero
    (cfs_mac_tag).
"""
import argparse
import hashlib
import struct
import sys

MASK64 = (1 << 64) - 1
MASK32 = (1 << 32) - 1


# --- Argon2id ---------------------------------------------------------------------------

def _hprime(x, t):
    if t <= 64:
        return hashlib.blake2b(struct.pack("<I", t) + x, digest_size=t).digest()
    r = (t + 31) // 32 - 2
    v = hashlib.blake2b(struct.pack("<I", t) + x).digest()
    out = v[:32]
    for _ in range(r - 1):
        v = hashlib.blake2b(v).digest()
        out += v[:32]
    v = hashlib.blake2b(v, digest_size=t - 32 * r).digest()
    return out + v


def _rotr(x, n):
    return ((x >> n) | (x << (64 - n))) & MASK64


def _gb(v, a, b, c, d):
    v[a] = (v[a] + v[b] + 2 * (v[a] & MASK32) * (v[b] & MASK32)) & MASK64
    v[d] = _rotr(v[d] ^ v[a], 32)
    v[c] = (v[c] + v[d] + 2 * (v[c] & MASK32) * (v[d] & MASK32)) & MASK64
    v[b] = _rotr(v[b] ^ v[c], 24)
    v[a] = (v[a] + v[b] + 2 * (v[a] & MASK32) * (v[b] & MASK32)) & MASK64
    v[d] = _rotr(v[d] ^ v[a], 16)
    v[c] = (v[c] + v[d] + 2 * (v[c] & MASK32) * (v[d] & MASK32)) & MASK64
    v[b] = _rotr(v[b] ^ v[c], 63)


def _p(v, idx):
    w = [v[i] for i in idx]
    _gb(w, 0, 4, 8, 12)
    _gb(w, 1, 5, 9, 13)
    _gb(w, 2, 6, 10, 14)
    _gb(w, 3, 7, 11, 15)
    _gb(w, 0, 5, 10, 15)
    _gb(w, 1, 6, 11, 12)
    _gb(w, 2, 7, 8, 13)
    _gb(w, 3, 4, 9, 14)
    for k, i in enumerate(idx):
        v[i] = w[k]


_ROWS = [list(range(16 * i, 16 * i + 16)) for i in range(8)]
_COLS = [[2 * i + 16 * j + k for j in range(8) for k in (0, 1)] for i in range(8)]


def _fill(prev, ref, cur, xor):
    r = [a ^ b for a, b in zip(ref, prev)]
    tmp = list(r)
    if xor:
        tmp = [a ^ b for a, b in zip(tmp, cur)]
    for idx in _ROWS:
        _p(r, idx)
    for idx in _COLS:
        _p(r, idx)
    return [a ^ b for a, b in zip(tmp, r)]


def _words(b):
    return list(struct.unpack("<128Q", b))


def argon2id(pwd, salt, t, m, p, outlen, secret=b"", ad=b""):
    h = hashlib.blake2b()
    for x in (p, outlen, m, t, 0x13, 2):
        h.update(struct.pack("<I", x))
    for x in (pwd, salt, secret, ad):
        h.update(struct.pack("<I", len(x)) + x)
    h0 = h.digest()
    mm = m // (4 * p) * (4 * p)
    q = mm // p
    seg = q // 4
    mem = [None] * mm
    for lane in range(p):
        for j in range(2):
            mem[lane * q + j] = _words(_hprime(h0 + struct.pack("<II", j, lane), 1024))
    zero = [0] * 128
    for pas in range(t):
        for sl in range(4):
            for lane in range(p):
                indep = pas == 0 and sl < 2
                inp = [0] * 128
                addr = None
                if indep:
                    inp[0:6] = [pas, lane, sl, mm, t, 2]
                start = 0
                if pas == 0 and sl == 0:
                    start = 2
                    if indep:
                        inp[6] += 1
                        addr = _fill(zero, _fill(zero, inp, zero, False), zero, False)
                cur = lane * q + sl * seg + start
                prev = cur - 1 if cur % q != 0 else cur + q - 1
                for i in range(start, seg):
                    if cur % q == 1:
                        prev = cur - 1
                    if indep:
                        if i % 128 == 0:
                            inp[6] += 1
                            addr = _fill(zero, _fill(zero, inp, zero, False), zero, False)
                        pseudo = addr[i % 128]
                    else:
                        pseudo = mem[prev][0]
                    ref_lane = (pseudo >> 32) % p
                    if pas == 0 and sl == 0:
                        ref_lane = lane
                    same = ref_lane == lane
                    if pas == 0:
                        if sl == 0:
                            area = i - 1
                        elif same:
                            area = sl * seg + i - 1
                        else:
                            area = sl * seg + (-1 if i == 0 else 0)
                    else:
                        area = q - seg + i - 1 if same else q - seg + (-1 if i == 0 else 0)
                    rel = pseudo & MASK32
                    rel = (rel * rel) >> 32
                    rel = area - 1 - ((area * rel) >> 32)
                    begin = 0 if pas == 0 or sl == 3 else (sl + 1) * seg
                    ref = (begin + rel) % q
                    mem[cur] = _fill(mem[prev], mem[ref_lane * q + ref], mem[cur] or zero, pas != 0)
                    cur += 1
                    prev += 1
    final = mem[q - 1]
    for lane in range(1, p):
        final = [a ^ b for a, b in zip(final, mem[lane * q + q - 1])]
    return _hprime(struct.pack("<128Q", *final), outlen)


# --- ChaCha20-Poly1305 (RFC 8439) --------------------------------------------------------

def _qr(s, a, b, c, d):
    s[a] = (s[a] + s[b]) & MASK32; s[d] ^= s[a]; s[d] = ((s[d] << 16) | (s[d] >> 16)) & MASK32
    s[c] = (s[c] + s[d]) & MASK32; s[b] ^= s[c]; s[b] = ((s[b] << 12) | (s[b] >> 20)) & MASK32
    s[a] = (s[a] + s[b]) & MASK32; s[d] ^= s[a]; s[d] = ((s[d] << 8) | (s[d] >> 24)) & MASK32
    s[c] = (s[c] + s[d]) & MASK32; s[b] ^= s[c]; s[b] = ((s[b] << 7) | (s[b] >> 25)) & MASK32


def _chacha_block(key, counter, nonce):
    st = [0x61707865, 0x3320646e, 0x79622d32, 0x6b206574] + list(struct.unpack("<8I", key)) + [counter] + \
        list(struct.unpack("<3I", nonce))
    w = list(st)
    for _ in range(10):
        _qr(w, 0, 4, 8, 12); _qr(w, 1, 5, 9, 13); _qr(w, 2, 6, 10, 14); _qr(w, 3, 7, 11, 15)
        _qr(w, 0, 5, 10, 15); _qr(w, 1, 6, 11, 12); _qr(w, 2, 7, 8, 13); _qr(w, 3, 4, 9, 14)
    return struct.pack("<16I", *[(a + b) & MASK32 for a, b in zip(w, st)])


def chacha20_xor(key, counter, nonce, data):
    out = bytearray()
    for i in range(0, len(data), 64):
        ks = _chacha_block(key, counter + i // 64, nonce)
        out += bytes(a ^ b for a, b in zip(data[i:i + 64], ks))
    return bytes(out)


def poly1305(key, msg):
    r = int.from_bytes(key[:16], "little") & 0x0ffffffc0ffffffc0ffffffc0fffffff
    s = int.from_bytes(key[16:], "little")
    p = (1 << 130) - 5
    acc = 0
    for i in range(0, len(msg), 16):
        n = int.from_bytes(msg[i:i + 16] + b"\x01", "little")
        acc = (acc + n) * r % p
    return ((acc + s) & ((1 << 128) - 1)).to_bytes(16, "little")


def _pad16(b):
    return b + bytes(-len(b) % 16)


def aead_open(key, nonce, aad, ct, tag):
    otk = _chacha_block(key, 0, nonce)[:32]
    mac = poly1305(otk, _pad16(aad) + _pad16(ct) + struct.pack("<QQ", len(aad), len(ct)))
    if mac != tag:
        return None
    return chacha20_xor(key, 1, nonce, ct)


# --- a pool's slots and its MAC ---------------------------------------------------------

SLOT_PASSPHRASE, SLOT_KEYFILE = 1, 2
KEYS2 = struct.Struct("<II16s16x")          # version, nslots, uuid, reserved[2]
SLOT = struct.Struct("<IIII16s32s16sQQ")    # struct cfs_keyslot, 96 bytes


def slot_aad(uuid, index, kind, t, m, lanes, salt):
    return uuid + struct.pack("<5I", index, kind, t, m, lanes) + salt


def slot_nonce(index):
    return b"cfs-slot" + struct.pack("<I", index)


def slots_of(key_block):
    """(uuid, [(index, kind, t, m, lanes, salt, wrapped, tag)]) of a version-2
    key block's payload (the block after its 32-byte header)."""
    version, nslots, uuid = KEYS2.unpack_from(key_block, 32)
    if version != 2:
        raise ValueError(f"key block version {version}")
    out = []
    for i in range(nslots):
        kind, t, m, lanes, salt, wrapped, tag, _, _ = SLOT.unpack_from(key_block, 32 + KEYS2.size + i * SLOT.size)
        out.append((i, kind, t, m, lanes, salt, wrapped, tag))
    return uuid, out


def open_master(key_block, passphrase, max_kib=1024):
    """The master key, from the first passphrase slot cheap enough to open
    here (at most `max_kib` of Argon2id memory) that the passphrase opens."""
    uuid, slots = slots_of(key_block)
    for index, kind, t, m, lanes, salt, wrapped, tag in slots:
        if kind != SLOT_PASSPHRASE or m > max_kib:
            continue
        k = argon2id(passphrase, salt, t, m, lanes, 32)
        master = aead_open(k, slot_nonce(index), slot_aad(uuid, index, kind, t, m, lanes, salt), wrapped, tag)
        if master is not None:
            return master
    raise ValueError("no slot cheap enough to open here opens with that passphrase")


def mac_key(master):
    return hashlib.blake2b(b"cosmofs metadata MAC v1", key=master, digest_size=32).digest()


def mac_tag(key, domain, block, off):
    b = bytes(block[:off]) + bytes(8) + bytes(block[off + 8:])
    return hashlib.blake2b(domain.encode() + b, key=key, digest_size=8).digest()


def self_test():
    # RFC 9106 section 5.3.
    got = argon2id(bytes([1]) * 32, bytes([2]) * 16, 3, 32, 4, 32, bytes([3]) * 8, bytes([4]) * 12)
    assert got.hex() == "0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659", got.hex()
    # RFC 8439 section 2.5.2.
    key = bytes.fromhex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b")
    assert poly1305(key, b"Cryptographic Forum Research Group").hex() == "a8061dc1305136c6c22b8baf0c0127a9"
    # RFC 8439 section 2.4.2.
    k = bytes(range(32))
    n = bytes.fromhex("000000000000004a00000000")
    pt = b"Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it."
    assert chacha20_xor(k, 1, n, pt)[:16].hex() == "6e2e359a2568f98041ba0728dd0d6981"
    print("cosmofs_key: self-test ok (Argon2id RFC 9106, Poly1305 and ChaCha20 RFC 8439)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true", help="the RFC vectors")
    a = ap.parse_args()
    if not a.self_test:
        ap.error("this is a module (tools/cosmofs-damage.py imports it); --self-test checks it")
    self_test()
    return 0


if __name__ == "__main__":
    sys.exit(main())
