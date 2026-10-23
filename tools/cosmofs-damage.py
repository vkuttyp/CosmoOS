#!/usr/bin/env python3
"""Damage a cosmofs pool on an unmounted image, one checker finding class
at a time (roadmap 1.2 B2; docs/kernel-services/filesystem/cosmofs/
testing.md, "test-fsck").

    cosmofs-damage.py IMAGE CLASS [--out OUT] [--passphrase P]
    cosmofs-damage.py --list
    cosmofs-damage.py IMAGE --describe [--passphrase P]
    cosmofs-damage.py --flip-mirror COPY0 COPY1 --path PATH [--count N]

IMAGE is a GPT disk whose second partition is the CosmoOS root (what
cosmo-install writes) or a bare cosmofs device. The damage is written
into OUT (a copy of IMAGE) or into IMAGE itself. Every structure the
damage touches is sealed again -- a metadata block's header CRC, a
directory block's entry in its file's checksum tree, the superblock's CRC
-- so the pool is what a buggy kernel would have written, not what a bad
disk would have returned: the checker sees a structure, not a checksum.

Both superblock slots are then made to name the damaged tree (the older
slot gets the newer one's root at generation - 1), so the mount's
older-slot fallback cannot step past the damage to a tree without it.

Classes (the names fsck.cosmofs prints; `--list` shows each with what it
damages and the classes the checker is expected to report):

  leaked free-in-use cross-linked nlink orphan dangling bad-entry
  counter cycle-extent cycle-deadlist cycle-snaplist cycle-free-record
  cycle-orphan-record cycle-directory unreadable extent-order
  extent-overlap dup-name snap-identity rollback-record hold root-type
  superblock

The named files are the ones test-fsck's preparation boot makes under
/fsck (tests/boot/fsck_test.py); the snapshot and deadlist classes need
the snapshot it takes. --flip-mirror is the mirror case: COPY0 and COPY1
are the two devices of a two-way mirror, and the first N data blocks of
PATH are flipped on COPY1 only.

An encrypted root (format 14, roadmap 1.2 B5) is damaged the same way,
with its own keys: --passphrase opens a slot cheap enough to open in
Python (tools/cosmofs_key.py; test-fsck adds one with `cosmo-key add
--memory 8 --passes 1`), and every block it touches is sealed with the
pool's metadata MAC instead of a CRC; directory blocks are decrypted and
encrypted again with their file's key, a fresh nonce and their entry in
the checksum tree. The kernel then sees the structure, as on a plain pool,
not a forgery.

Exit 0 when the damage was written, 2 for a usage error, 1 when the image
does not have what the class needs.
"""
import argparse
import os
import shutil
import struct
import sys
import uuid as uuidlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cosmofs_key  # noqa: E402

BLOCK = 4096
MHDR = 32
MHDR_MAGIC = 0x4D534643
PTRS = (BLOCK - MHDR) // 8                      # 508
INODES_PER_BLOCK = (BLOCK - MHDR) // 256        # 15
BITS_PER_BITMAP = (BLOCK - MHDR) * 8            # 32512
CSUMS_PER_BLOCK = (BLOCK - MHDR) // 4           # 1016
DIRENT = 64
NAME_MAX = 47
EXTENTS_PER_BLOCK = (BLOCK - MHDR - 8) // 16    # 253
SNAPS_PER_BLOCK2 = (BLOCK - MHDR - 16) // 128   # 31
DEAD_PER_BLOCK = (BLOCK - MHDR - 16) // 8
DVA_SHIFT = 56
TYPE_REG, TYPE_DIR = 1, 2
EXT_COMPRESSED = 1 << 31
K_IMAP1, K_IMAP0, K_INODES, K_ALLOCIDX, K_BITMAP, K_EXTENTS = 1, 2, 3, 4, 5, 6
K_CSUMIDX, K_CSUM, K_SNAPLIST, K_DEADLIST, K_MEMBERS = 7, 8, 9, 10, 11
K_FREELOG, K_ORPHAN, K_SNAPLIST2, K_ROLLBACK = 13, 14, 15, 16
ROLLBACK_MAGIC = 0x42524643
ROOT_TYPE = uuidlib.UUID("c9b09224-e00d-418e-846a-9f1d9a61bfd5")
SS = 512

# class: (what is damaged, the checker classes it is expected to report)
CLASSES = {
    "leaked": ("a free block's bitmap bit set", ["leaked"]),
    "free-in-use": ("/fsck/a's data block's bitmap bit cleared", ["free-in-use"]),
    "cross-linked": ("/fsck/a's first run pointed at /fsck/sparse's last block", ["cross-linked", "leaked"]),
    "nlink": ("/fsck/a's link count raised by one", ["nlink"]),
    "orphan": ("/fsck/b's entry removed from /fsck", ["orphan"]),
    "dangling": ("/fsck/c's entry pointed past the inode map's high-water mark", ["dangling", "orphan"]),
    "bad-entry": ("/fsck/d's entry given a 200-byte name length", ["bad-entry", "orphan"]),
    "counter": ("the superblock's inode count raised by one", ["counter"]),
    "cycle-extent": ("/fsck/a given an extent chain whose block names itself", ["cycle"]),
    "cycle-deadlist": ("the first snapshot's deadlist made to name itself", ["cycle"]),
    "cycle-snaplist": ("the snapshot list's head made to name itself", ["cycle"]),
    "cycle-free-record": ("the free record's head made to name itself", ["free-record"]),
    "cycle-orphan-record": ("an orphan record whose block names itself", ["orphan-record"]),
    "cycle-directory": ("/fsck/dir given an entry naming /fsck, its parent", ["bad-entry", "nlink"]),
    "unreadable": ("/fsck/dir's directory block rotted under its checksum", ["unreadable"]),
    "extent-order": ("/fsck/sparse's two runs swapped", ["extent-order"]),
    "extent-overlap": ("/fsck/sparse's first run made two new blocks, its second moved inside them", ["extent-overlap", "leaked"]),
    "dup-name": ("/fsck/f renamed e in place, beside /fsck/e", ["dup-name"]),
    "snap-identity": ("the first snapshot's uuid zeroed", ["snap-identity"]),
    "rollback-record": ("a pending rollback record naming a snapshot that is not there", ["rollback-record"]),
    "hold": ("a hold naming a safety snapshot that is not there", ["hold"]),
    "root-type": ("the root inode's mode made a regular file's", ["root"]),
    "superblock": ("the older superblock slot given another pool's uuid", ["superblock"]),
}


class Bad(Exception):
    pass


# --- CRC32C (Castagnoli), as the kernel's crc32c() ------------------------------

def _table():
    t = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
        t.append(c)
    return t


_T = _table()


def crc32c(data, crc=0):
    crc ^= 0xFFFFFFFF
    t = _T
    for b in data:
        crc = t[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


def block_crc(block, off):
    """The kernel's block_crc: the block with the 4-byte field at `off` zero."""
    return crc32c(bytes(block[:off]) + b"\0\0\0\0" + bytes(block[off + 4:]))


# --- the pool on an image ---------------------------------------------------------

def find_root(f, size):
    """Byte offset and length of the cosmofs on this image."""
    f.seek(0)
    head = f.read(2 * BLOCK)
    if head[:8] == b"COSMOFS1" or head[BLOCK:BLOCK + 8] == b"COSMOFS1":
        return 0, size
    f.seek(SS)
    h = f.read(92)
    if h[:8] != b"EFI PART":
        raise Bad("neither a cosmofs device nor a GPT disk")
    entries_lba = int.from_bytes(h[72:80], "little")
    count = int.from_bytes(h[80:84], "little")
    esize = int.from_bytes(h[84:88], "little")
    f.seek(entries_lba * SS)
    arr = f.read(count * esize)
    for i in range(count):
        e = arr[i * esize:(i + 1) * esize]
        if uuidlib.UUID(bytes_le=bytes(e[:16])) == ROOT_TYPE:
            first = int.from_bytes(e[32:40], "little")
            last = int.from_bytes(e[40:48], "little")
            return first * SS, (last - first + 1) * SS
    raise Bad("the GPT has no CosmoOS root partition")


SUPER_FMT = "<8sIIQQQQQQQQQQ16sQQQ16sQII"
SUPER_FIELDS = ("magic version block_size total_blocks generation imap_root alloc_root next_ino inode_count "
                "free_blocks csum_root snap_root members uuid key_root free_root orphan_root label "
                "rollback_root crc pad").split()
SUPER_CRC_OFF = struct.calcsize(SUPER_FMT) - 8

INODE_FMT = "<IIIIQQQQQ" + "QII" * 10 + "QQIIQII"


class Inode:
    def __init__(self, raw):
        v = struct.unpack(INODE_FMT, raw)
        (self.mode, self.nlink, self.uid, self.gid, self.size, self.mtime, self.ctime, self.generation,
         self.ino) = v[:9]
        self.direct = [list(v[9 + 3 * i:12 + 3 * i]) for i in range(10)]   # [start, count, lblk]
        (self.indirect, self.parent, self.csum_algo, self.csum_pad, self.csum_root, self.compress_algo,
         self.reserved) = v[39:]

    def pack(self):
        flat = []
        for d in self.direct:
            flat += d
        return struct.pack(INODE_FMT, self.mode, self.nlink, self.uid, self.gid, self.size, self.mtime, self.ctime,
                           self.generation, self.ino, *flat, self.indirect, self.parent, self.csum_algo,
                           self.csum_pad, self.csum_root, self.compress_algo, self.reserved)

    def runs(self):
        return [d for d in self.direct if d[1] & 0xFFFF and d[1]]


AEAD_PER_BLOCK = (BLOCK - MHDR) // 32           # 127
K_KEYS = 12


class Pool:
    def __init__(self, path, passphrase=None):
        self.f = open(path, "r+b")
        size = os.fstat(self.f.fileno()).st_size
        self.base, self.size = find_root(self.f, size)
        self.master = self.mac = None
        self.mac_mode = False
        raw = [self.read(s) for s in (0, 1)]
        heads = [dict(zip(SUPER_FIELDS, struct.unpack_from(SUPER_FMT, r))) for r in raw if r[:8] == b"COSMOFS1"]
        if any(h["version"] >= 14 and h["key_root"] for h in heads):
            # An encrypted root: its keys first, from the key block a slot
            # names -- read unverified, as the kernel's KEY_INFO reads it;
            # the slot authenticates itself when it opens.
            if passphrase is None:
                raise Bad("an encrypted pool: --passphrase is needed")
            h = max(heads, key=lambda x: x["generation"])
            self.master = cosmofs_key.open_master(bytes(self.read(h["key_root"])), passphrase.encode())
            self.mac = cosmofs_key.mac_key(self.master)
            self.mac_mode = True
        a, b = self.super_at(0), self.super_at(1)
        good = [s for s in (a, b) if s is not None]
        if not good:
            raise Bad("no superblock slot verifies")
        self.slots = (a, b)
        self.sb = max(good, key=lambda s: s["generation"])
        self.slot = 0 if self.sb is a else 1
        self.v = self.sb["version"]

    # raw blocks of member 0
    def read(self, blk):
        if blk >> DVA_SHIFT:
            raise Bad(f"block {blk:#x} is on member {blk >> DVA_SHIFT}; only member 0 is handled")
        self.f.seek(self.base + blk * BLOCK)
        b = self.f.read(BLOCK)
        if len(b) != BLOCK:
            raise Bad(f"block {blk} is past the image")
        return bytearray(b)

    def write(self, blk, data):
        assert len(data) == BLOCK
        self.f.seek(self.base + blk * BLOCK)
        self.f.write(bytes(data))

    def super_at(self, slot):
        b = self.read(slot)
        if b[:8] != b"COSMOFS1":
            return None
        v = dict(zip(SUPER_FIELDS, struct.unpack_from(SUPER_FMT, b)))
        if self.mac_mode:
            if cosmofs_key.mac_tag(self.mac, "S", b, SUPER_CRC_OFF) != bytes(b[SUPER_CRC_OFF:SUPER_CRC_OFF + 8]):
                return None
        elif block_crc(b, SUPER_CRC_OFF) != v["crc"]:
            return None
        v["_raw"] = b
        return v

    def put_super(self, slot, sb):
        b = bytearray(sb["_raw"])
        vals = [sb[k] for k in SUPER_FIELDS]
        struct.pack_into(SUPER_FMT, b, 0, *vals)
        if self.mac_mode:
            b[SUPER_CRC_OFF:SUPER_CRC_OFF + 8] = cosmofs_key.mac_tag(self.mac, "S", b, SUPER_CRC_OFF)
        else:
            struct.pack_into("<I", b, SUPER_CRC_OFF, block_crc(b, SUPER_CRC_OFF))
        self.write(slot, b)

    def commit_super(self):
        """The newest slot as edited, and the other naming the same tree one
        generation older: the mount's fallback has nowhere else to go."""
        self.put_super(self.slot, self.sb)
        other = dict(self.sb)
        other["generation"] = self.sb["generation"] - 1
        self.put_super(1 - self.slot, other)

    # metadata blocks
    def meta(self, blk, kind):
        b = self.read(blk)
        magic, k, gen, blkno, crc, algo = struct.unpack_from("<IIQQII", b)
        sealed = (cosmofs_key.mac_tag(self.mac, "H", b, 24) == bytes(b[24:32]) if self.mac_mode
                  else block_crc(b, 24) == crc)
        if magic != MHDR_MAGIC or k != kind or blkno != blk or not sealed:
            raise Bad(f"block {blk:#x} is not a sound metadata block of kind {kind}")
        return b

    def seal(self, blk, b):
        if self.mac_mode:
            b[24:32] = cosmofs_key.mac_tag(self.mac, "H", b, 24)
        else:
            struct.pack_into("<I", b, 24, block_crc(b, 24))
        self.write(blk, b)

    # an encrypted file's blocks (format 7; cosmofs_crypt.c, cosmofs.c seal_block)
    def file_key(self, ino):
        return cosmofs_key.chacha20_xor(self.master, 0, struct.pack("<Q", ino) + bytes(4), bytes(32))

    def aead_entry(self, inode, lblk):
        """(the CSUM block, the entry's offset in it) of an encrypted file's
        block `lblk`: a one-level index, as a directory's is."""
        idx = self.meta(inode.csum_root, K_CSUMIDX)
        leaf = struct.unpack_from("<Q", idx, MHDR + 8 * (lblk // AEAD_PER_BLOCK))[0]
        return leaf, MHDR + 32 * (lblk % AEAD_PER_BLOCK)

    def data_read(self, inode, lblk, pb):
        b = self.read(pb)
        if inode.csum_algo != 2:
            return b
        leaf, off = self.aead_entry(inode, lblk)
        lb = self.meta(leaf, K_CSUM)
        tag, nonce = bytes(lb[off:off + 16]), bytes(lb[off + 16:off + 28])
        plain = cosmofs_key.aead_open(self.file_key(inode.ino), nonce, struct.pack("<QQ", inode.ino, lblk),
                                      bytes(b), tag)
        if plain is None:
            raise Bad(f"inode {inode.ino} block {lblk} does not open")
        return bytearray(plain)

    def new_meta(self, kind):
        """A free block, allocated, sealed as an empty block of `kind`."""
        blk = self.free_block()
        self.set_bit(blk, True)
        b = bytearray(BLOCK)
        struct.pack_into("<IIQQII", b, 0, MHDR_MAGIC, kind, self.sb["generation"], blk, 0, 1)
        return blk, b

    # allocation
    def members(self):
        if self.v < 4:
            return [{"alloc_root": self.sb["alloc_root"], "first_usable": 2, "nblocks": self.sb["total_blocks"]}]
        b = self.meta(self.sb["members"], K_MEMBERS)
        count = struct.unpack_from("<Q", b, MHDR)[0]
        out = []
        for i in range(count):
            u, nb, fu, ar, fb, fl, cp, _ = struct.unpack_from("<16sQQQQIIQ", b, MHDR + 8 + 64 * i)
            out.append({"nblocks": nb, "first_usable": fu, "alloc_root": ar})
        return out

    def bitmap_at(self, blk):
        m = self.members()[0]
        idx = self.meta(m["alloc_root"], K_ALLOCIDX)
        chunk = struct.unpack_from("<Q", idx, MHDR + 8 * (blk // BITS_PER_BITMAP))[0]
        return chunk, blk % BITS_PER_BITMAP

    def bit(self, blk):
        chunk, i = self.bitmap_at(blk)
        b = self.meta(chunk, K_BITMAP)
        return (b[MHDR + i // 8] >> (i % 8)) & 1

    def set_bit(self, blk, on):
        chunk, i = self.bitmap_at(blk)
        b = self.meta(chunk, K_BITMAP)
        if on:
            b[MHDR + i // 8] |= 1 << (i % 8)
        else:
            b[MHDR + i // 8] &= ~(1 << (i % 8)) & 0xFF
        self.seal(chunk, b)

    def free_pair(self):
        m = self.members()[0]
        for blk in range(m["nblocks"] - 2, m["first_usable"], -1):
            if not self.bit(blk) and not self.bit(blk + 1):
                return blk
        raise Bad("no two adjacent free blocks")

    def free_block(self):
        m = self.members()[0]
        for blk in range(m["nblocks"] - 1, m["first_usable"], -1):
            if not self.bit(blk):
                return blk
        raise Bad("no free block")

    # inodes
    def inode_place(self, ino):
        bi = ino // INODES_PER_BLOCK
        l1 = self.meta(self.sb["imap_root"], K_IMAP1)
        l0blk = struct.unpack_from("<Q", l1, MHDR + 8 * (bi // PTRS))[0]
        l0 = self.meta(l0blk, K_IMAP0)
        ib = struct.unpack_from("<Q", l0, MHDR + 8 * (bi % PTRS))[0]
        return ib, MHDR + 256 * (ino % INODES_PER_BLOCK)

    def inode(self, ino):
        ib, off = self.inode_place(ino)
        b = self.meta(ib, K_INODES)
        return Inode(bytes(b[off:off + 256]))

    def put_inode(self, ino, inode):
        ib, off = self.inode_place(ino)
        b = self.meta(ib, K_INODES)
        b[off:off + 256] = inode.pack()
        self.seal(ib, b)

    # files and directories
    def all_runs(self, inode):
        runs = [list(d) for d in inode.direct if d[1]]
        at, guard = inode.indirect, 0
        while at and guard < 32:
            b = self.meta(at, K_EXTENTS)
            nxt = struct.unpack_from("<Q", b, MHDR)[0]
            for i in range(EXTENTS_PER_BLOCK):
                s, c, l = struct.unpack_from("<QII", b, MHDR + 8 + 16 * i)
                if c:
                    runs.append([s, c, l])
            at, guard = nxt, guard + 1
        return runs

    def map(self, inode, lblk):
        for s, c, l in self.all_runs(inode):
            if c & EXT_COMPRESSED:
                continue
            if l <= lblk < l + c:
                return s + (lblk - l)
        return None

    def dir_entries(self, ino):
        d = self.inode(ino)
        out = []
        for lblk in range((d.size + BLOCK - 1) // BLOCK):
            pb = self.map(d, lblk)
            if pb is None:
                continue
            b = self.data_read(d, lblk, pb)
            for s in range(BLOCK // DIRENT):
                e_ino, typ, nl = struct.unpack_from("<QBB", b, s * DIRENT)
                if e_ino:
                    name = bytes(b[s * DIRENT + 16:s * DIRENT + 16 + min(nl, 48)]).decode(errors="replace")
                    out.append((name, e_ino, typ, lblk, pb, s))
        return out

    def lookup(self, path):
        ino = 1
        for part in [p for p in path.split("/") if p]:
            hit = [e for e in self.dir_entries(ino) if e[0] == part]
            if not hit:
                raise Bad(f"{path}: no entry {part!r}")
            ino = hit[0][1]
        return ino

    def entry(self, path):
        parent, name = path.rsplit("/", 1)
        pino = self.lookup(parent or "/")
        for e in self.dir_entries(pino):
            if e[0] == name:
                return pino, e
        raise Bad(f"{path}: not found")

    def write_dir_block(self, dino, lblk, pb, data):
        """A directory block written in place, its checksum entry with it."""
        d = self.inode(dino)
        if d.csum_algo == 2:
            # Encrypted: a fresh nonce, the tag over the ciphertext and the
            # block's place, and the keyless CRC beside it (seal_block).
            nonce = os.urandom(12)
            ct = cosmofs_key.chacha20_xor(self.file_key(dino), 1, nonce, bytes(data))
            aad = struct.pack("<QQ", dino, lblk)
            otk = cosmofs_key._chacha_block(self.file_key(dino), 0, nonce)[:32]
            tag = cosmofs_key.poly1305(otk, cosmofs_key._pad16(aad) + cosmofs_key._pad16(ct) +
                                       struct.pack("<QQ", len(aad), len(ct)))
            self.write(pb, ct)
            leaf, off = self.aead_entry(d, lblk)
            lb = self.meta(leaf, K_CSUM)
            lb[off:off + 32] = tag + nonce + struct.pack("<I", crc32c(ct))
            self.seal(leaf, lb)
            return
        self.write(pb, data)
        if d.csum_algo != 1:
            raise Bad(f"directory {dino} is not CRC32C-checksummed")
        idx = self.meta(d.csum_root, K_CSUMIDX)
        leaf = struct.unpack_from("<Q", idx, MHDR + 8 * (lblk // CSUMS_PER_BLOCK))[0]
        lb = self.meta(leaf, K_CSUM)
        struct.pack_into("<I", lb, MHDR + 4 * (lblk % CSUMS_PER_BLOCK), crc32c(bytes(data)))
        self.seal(leaf, lb)

    # snapshots (format 12)
    def snap_head(self):
        if self.v < 12 or not self.sb["snap_root"]:
            raise Bad("no version-12 snapshot list (test-fsck's preparation takes a snapshot)")
        return self.sb["snap_root"], self.meta(self.sb["snap_root"], K_SNAPLIST2)

    def first_snapshot(self):
        head, b = self.snap_head()
        for i in range(SNAPS_PER_BLOCK2):
            off = MHDR + 16 + 128 * i
            if struct.unpack_from("<Q", b, off + 56)[0]:   # id
                return head, b, off
        raise Bad("the snapshot list is empty")


# --- the damage -------------------------------------------------------------------

def first_run(p, path):
    ino = p.lookup(path)
    inode = p.inode(ino)
    runs = inode.runs()
    if not runs or runs[0][1] & EXT_COMPRESSED:
        raise Bad(f"{path} has no plain first run")
    return ino, inode


def damage(p, cls):
    if cls == "leaked":
        blk = p.free_block()
        p.set_bit(blk, True)
        return f"block 0:{blk}"
    if cls == "free-in-use":
        _, inode = first_run(p, "/fsck/a")
        p.set_bit(inode.direct[0][0], False)
        return f"block 0:{inode.direct[0][0]}"
    if cls == "cross-linked":
        ino, inode = first_run(p, "/fsck/a")
        sp = p.inode(p.lookup("/fsck/sparse")).runs()
        victim = sp[-1][0]
        inode.direct[0][0] = victim
        p.put_inode(ino, inode)
        return f"inode {ino} run 0 -> block 0:{victim}"
    if cls == "nlink":
        ino = p.lookup("/fsck/a")
        inode = p.inode(ino)
        inode.nlink += 1
        p.put_inode(ino, inode)
        return f"inode {ino} nlink {inode.nlink}"
    if cls in ("orphan", "dangling", "bad-entry", "dup-name"):
        name = {"orphan": "/fsck/b", "dangling": "/fsck/c", "bad-entry": "/fsck/d", "dup-name": "/fsck/f"}[cls]
        pino, (n, e_ino, typ, lblk, pb, s) = p.entry(name)
        b = p.data_read(p.inode(pino), lblk, pb)
        if cls == "orphan":
            b[s * DIRENT:(s + 1) * DIRENT] = bytes(DIRENT)
        elif cls == "dangling":
            struct.pack_into("<Q", b, s * DIRENT, p.sb["next_ino"] + 3)
        elif cls == "bad-entry":
            b[s * DIRENT + 9] = 200
        else:
            b[s * DIRENT + 9] = 1
            b[s * DIRENT + 16:s * DIRENT + 64] = b"e" + bytes(47)
        p.write_dir_block(pino, lblk, pb, b)
        return f"entry {name} (inode {e_ino}) in directory {pino}"
    if cls == "counter":
        p.sb["inode_count"] += 1
        return f"inode_count {p.sb['inode_count']}"
    if cls == "cycle-extent":
        ino, inode = first_run(p, "/fsck/a")
        if inode.indirect:
            raise Bad("/fsck/a already has a chain")
        blk, b = p.new_meta(K_EXTENTS)
        struct.pack_into("<Q", b, MHDR, blk)
        p.seal(blk, b)
        inode.indirect = blk
        p.put_inode(ino, inode)
        return f"inode {ino} chain 0:{blk} -> itself"
    if cls == "cycle-deadlist":
        head, b, off = p.first_snapshot()
        dl = struct.unpack_from("<Q", b, off + 40)[0]
        if dl:
            d = p.meta(dl, K_DEADLIST)
            struct.pack_into("<Q", d, MHDR, dl)
            p.seal(dl, d)
        else:
            dl, d = p.new_meta(K_DEADLIST)
            struct.pack_into("<Q", d, MHDR, dl)
            p.seal(dl, d)
            struct.pack_into("<Q", b, off + 40, dl)
            p.seal(head, b)
        return f"deadlist 0:{dl} -> itself"
    if cls == "cycle-snaplist":
        head, b = p.snap_head()
        struct.pack_into("<Q", b, MHDR, head)
        p.seal(head, b)
        return f"snapshot list 0:{head} -> itself"
    if cls == "cycle-free-record":
        fr = p.sb["free_root"]
        if p.v < 9 or not fr:
            raise Bad("no free record")
        b = p.meta(fr, K_FREELOG)
        struct.pack_into("<Q", b, MHDR, fr)
        p.seal(fr, b)
        return f"free record 0:{fr} -> itself"
    if cls == "cycle-orphan-record":
        if p.v < 10:
            raise Bad("no orphan record before version 10")
        blk, b = p.new_meta(K_ORPHAN)
        struct.pack_into("<QQ", b, MHDR, blk, 0)
        p.seal(blk, b)
        p.sb["orphan_root"] = blk
        return f"orphan record 0:{blk} -> itself"
    if cls == "cycle-directory":
        pino = p.lookup("/fsck/dir")
        up = p.lookup("/fsck")
        d = p.inode(pino)
        pb = p.map(d, 0)
        b = p.data_read(d, 0, pb)
        for s in range(BLOCK // DIRENT):
            if struct.unpack_from("<Q", b, s * DIRENT)[0] == 0:
                struct.pack_into("<QBB", b, s * DIRENT, up, TYPE_DIR, 2)
                b[s * DIRENT + 16:s * DIRENT + 18] = b"up"
                break
        else:
            raise Bad("/fsck/dir's first block is full")
        p.write_dir_block(pino, 0, pb, b)
        return f"directory {pino} entry 'up' -> {up}"
    if cls == "unreadable":
        d = p.inode(p.lookup("/fsck/dir"))
        pb = p.map(d, 0)
        b = p.read(pb)
        b[100] ^= 0xFF
        p.write(pb, b)                  # not re-sealed: the rot is the point
        return f"directory block 0:{pb}"
    if cls in ("extent-order", "extent-overlap"):
        ino = p.lookup("/fsck/sparse")
        inode = p.inode(ino)
        r = inode.runs()
        if cls == "extent-order":
            if len(r) < 2:
                raise Bad(f"/fsck/sparse needs two runs; its runs: {r}")
            inode.direct[0], inode.direct[1] = inode.direct[1], inode.direct[0]
            p.put_inode(ino, inode)
            return f"inode {ino} runs 0 and 1 swapped: {inode.direct[0]} {inode.direct[1]}"
        # A run of two blocks the second run starts inside. The allocator
        # lays a file out differently from one machine to the next, so
        # the two-block run is made here: two adjacent free blocks,
        # allocated, as run 0 (whose old block is then leaked, which a
        # repair frees), and run 1 moved to start at its second block.
        if len(r) < 2 or r[0][1] & EXT_COMPRESSED or r[1][1] & EXT_COMPRESSED:
            raise Bad(f"/fsck/sparse needs two plain runs; its runs: {r}")
        x = p.free_pair()
        p.set_bit(x, True)
        p.set_bit(x + 1, True)
        inode.direct[0] = [x, 2, inode.direct[0][2]]
        inode.direct[1][2] = inode.direct[0][2] + 1
        p.put_inode(ino, inode)
        return f"inode {ino} runs {inode.direct[0]} {inode.direct[1]} (run 0 on new blocks 0:{x}-{x + 1})"
    if cls == "snap-identity":
        head, b, off = p.first_snapshot()
        b[off + 64:off + 80] = bytes(16)
        p.seal(head, b)
        return f"snapshot id {struct.unpack_from('<Q', b, off + 56)[0]}"
    if cls in ("rollback-record", "hold"):
        if p.v < 12:
            raise Bad("no rollback record before version 12")
        if p.sb["rollback_root"]:
            raise Bad("a rollback record is already there")
        blk, b = p.new_meta(K_ROLLBACK)
        state = 1 if cls == "rollback-record" else 2
        rec = bytearray(struct.pack("<IIII16s16sQQQQ4QII", ROLLBACK_MAGIC, 1, state, 0, uuidlib.uuid4().bytes,
                                    uuidlib.uuid4().bytes, 0xFFF0, 0xFFF1, p.sb["generation"], 0, 0, 0, 0, 0, 0, 0))
        struct.pack_into("<I", rec, 112, crc32c(bytes(rec[:112]) + b"\0\0\0\0" + bytes(rec[116:])))
        b[MHDR:MHDR + len(rec)] = rec
        p.seal(blk, b)
        p.sb["rollback_root"] = blk
        return f"rollback record 0:{blk}, state {'pending' if state == 1 else 'held'}"
    if cls == "root-type":
        inode = p.inode(1)
        inode.mode = (TYPE_REG << 12) | (inode.mode & 0o7777)
        p.put_inode(1, inode)
        return "inode 1"
    if cls == "superblock":
        p.commit_super()
        other = dict(p.sb)
        other["generation"] = p.sb["generation"] - 1
        other["uuid"] = uuidlib.uuid4().bytes
        p.put_super(1 - p.slot, other)
        return f"slot {1 - p.slot} uuid {uuidlib.UUID(bytes=other['uuid'])}"
    raise Bad(f"unknown class {cls}")


def flip_mirror(copy0, copy1, path, count):
    p = Pool(copy0)
    inode = p.inode(p.lookup(path))
    blocks = []
    for s, c, l in p.all_runs(inode):
        n = (((c >> 16) & 0xFF) + 1) if c & EXT_COMPRESSED else c
        blocks += [s + i for i in range(n)]
    blocks = blocks[:count]
    if len(blocks) < count:
        raise Bad(f"{path} has {len(blocks)} block(s)")
    with open(copy1, "r+b") as f:
        for blk in blocks:
            f.seek(blk * BLOCK)
            b = bytearray(f.read(BLOCK))
            for i in range(64):
                b[i] ^= 0xFF
            f.seek(blk * BLOCK)
            f.write(b)
    return blocks


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", nargs="?")
    ap.add_argument("cls", nargs="?")
    ap.add_argument("--out")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--describe", action="store_true")
    ap.add_argument("--flip-mirror", nargs=2, metavar=("COPY0", "COPY1"))
    ap.add_argument("--path")
    ap.add_argument("--count", type=int, default=8)
    ap.add_argument("--passphrase", help="an encrypted root's: opens a slot cheap enough to open here")
    a = ap.parse_args()
    try:
        if a.list:
            for k, (what, expect) in CLASSES.items():
                print(f"{k:20} {','.join(expect):26} {what}")
            return 0
        if a.flip_mirror:
            if not a.path:
                ap.error("--flip-mirror needs --path")
            blocks = flip_mirror(a.flip_mirror[0], a.flip_mirror[1], a.path, a.count)
            print(f"flipped {len(blocks)} block(s) of {a.path} on {a.flip_mirror[1]}: "
                  + " ".join(f"0:{b}" for b in blocks))
            return 0
        if not a.image:
            ap.error("IMAGE is required")
        if a.describe:
            p = Pool(a.image, a.passphrase)
            sb = p.sb
            print(f"format {p.v}, generation {sb['generation']} (slot {p.slot}), {sb['total_blocks']} blocks, "
                  f"{sb['inode_count']} inodes, next_ino {sb['next_ino']}, snap_root {sb['snap_root']:#x}, "
                  f"free_root {sb['free_root']:#x}, orphan_root {sb['orphan_root']:#x}, "
                  f"rollback_root {sb['rollback_root']:#x}")
            return 0
        if a.cls not in CLASSES:
            ap.error(f"CLASS is one of: {' '.join(CLASSES)}")
        target = a.image
        if a.out:
            shutil.copyfile(a.image, a.out)
            target = a.out
        p = Pool(target, a.passphrase)
        what = damage(p, a.cls)
        if a.cls != "superblock":
            p.commit_super()
        p.f.close()
        print(f"damaged {a.cls}: {what}; expect {','.join(CLASSES[a.cls][1])}")
        return 0
    except Bad as e:
        print(f"cosmofs-damage: {a.cls or ''}: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
