#!/usr/bin/env python3
"""cosmofs-metadata-csum-probe.py -- cosmofs's metadata blocks record no
checksum algorithm.

Every metadata block carries a CRC32C in its 32-byte header
(`struct cfs_mhdr`), computed by `block_crc` and checked by `mhdr_check`
(kernel-services/filesystem/cosmofs/cosmofs_core.c). The algorithm is
hardcoded: `mhdr_seal` writes the header's one spare word (`pad`) as zero,
and the verifier applies CRC32C unconditionally. Data blocks are not like
this -- each inode's `csum_algo` says whether its blocks are CRC32C or a
Poly1305 tag (cosmofs_format.h, `CFS_CSUM_*`) -- so on an encrypted
filesystem the data is authenticated and the metadata is only CRC'd, with
no field even to declare otherwise.

This probe adds a self-test, `cosmofs-metadata-csum-id`, that seals a
metadata block and shows:
  - the header records no algorithm (the spare word is zero);
  - putting a would-be algorithm id in that word, with a valid CRC, still
    verifies -- the field selects nothing;
  - a block checksummed by a different rule is rejected by the *same*
    verdict as a one-bit corruption, so an algorithm change cannot be told
    from damage.

    python3 tools/cosmofs-metadata-csum-probe.py apply
    gmake ARCH=x86_64 test > run.txt 2>&1
    grep CFSPROBE out/x86_64-debug/boot-test.log
    python3 tools/cosmofs-metadata-csum-probe.py revert

`apply` and `revert` are those of tools/lockup-interrupted-probe.py.
"""

import hashlib
import os
import stat
import subprocess
import sys

TEST = 'kernel-services/filesystem/cosmofs/cosmofstest.c'
DECL = 'kernel/include/kernel/selftest.h'
REG = 'kernel/core/selftest.c'

# --- the demonstration test, at the end of the CONFIG_DEBUG region ---------
OLD_TEST_TAIL = """    kinfo("selftest: cosmofs-writeback: the thread committed generation %llu on its own", (unsigned long long)st1.generation);
    return engine_unmount(bd, reason);
}

#else
"""
NEW_TEST_TAIL = r"""    kinfo("selftest: cosmofs-writeback: the thread committed generation %llu on its own", (unsigned long long)st1.generation);
    return engine_unmount(bd, reason);
}

/* CFSPROBE: cosmofs's metadata blocks record no checksum algorithm. */
bool cfs_mhdr_ok(const void *block, uint64_t dva, uint32_t kind);
void cfs_mhdr_seal_raw(void *block, uint32_t kind, uint64_t dva, uint64_t generation);

static uint32_t cfsprobe_block_crc(const uint8_t *block)
{
    size_t off = offsetof(struct cfs_mhdr, crc);
    static const uint8_t zero4[4] = { 0 };
    uint32_t c = crc32c(block, off);
    c = crc32c_update(c, zero4, 4);
    return crc32c_update(c, block + off + 4, CFS_BLOCK - off - 4);
}

bool selftest_cosmofs_metadata_csum_id(const char **reason)
{
    uint8_t *block = kmalloc(CFS_BLOCK, 0);
    CHECK(block != NULL);
    memset(block, 0xa5, CFS_BLOCK);
    struct cfs_mhdr *h = (struct cfs_mhdr *)block;

    /* Seal a metadata block. The writer records no algorithm: the one spare
     * header word is left zero. */
    cfs_mhdr_seal_raw(block, CFS_KIND_INODES, 42, 7);
    bool sealed_ok = cfs_mhdr_ok(block, 42, CFS_KIND_INODES);
    unsigned sealed_algo = h->pad;

    /* Declare a different algorithm in the spare word, with a valid CRC. The
     * block still verifies: the verifier applies CRC32C regardless of the
     * field, so it selects nothing and a reader cannot learn one was meant. */
    h->pad = CFS_CSUM_POLY1305;
    h->crc = cfsprobe_block_crc(block);
    bool inert = cfs_mhdr_ok(block, 42, CFS_KIND_INODES);
    unsigned inert_algo = h->pad;

    /* A block checksummed by a different *rule* -- a CRC32C over the whole
     * block, the crc field included, rather than taken as zero -- is a
     * self-consistent checksum some other format could store, not a damaged
     * one. It is rejected: the verifier computes only its own rule. */
    cfs_mhdr_seal_raw(block, CFS_KIND_INODES, 42, 7);
    h->crc = crc32c(block, CFS_BLOCK);
    bool other_rule = cfs_mhdr_ok(block, 42, CFS_KIND_INODES);

    /* One flipped payload bit is rejected by the same verdict, so a different
     * rule cannot be told from corruption. */
    cfs_mhdr_seal_raw(block, CFS_KIND_INODES, 42, 7);
    block[CFS_MHDR_SIZE + 3] ^= 0x01u;
    bool corrupt = cfs_mhdr_ok(block, 42, CFS_KIND_INODES);

    kfree(block);   /* free before any CHECK can return */

    kprintf("CFSPROBE: sealed metadata header records algo %u (the spare word); verifies %d, always CRC32C\n",
            sealed_algo, (int)sealed_ok);
    kprintf("CFSPROBE: header declaring algo %u verifies %d under CRC32C -- the field is inert\n",
            inert_algo, (int)inert);
    kprintf("CFSPROBE: different-rule block verifies %d, one-bit-corrupt block verifies %d -- same verdict\n",
            (int)other_rule, (int)corrupt);
    CHECK(sealed_ok);
    CHECK(sealed_algo == 0);
    CHECK(inert);
    CHECK(!other_rule && !corrupt);
    kinfo("selftest: cosmofs-metadata-csum-id: metadata records no checksum algorithm; a different checksum rule is indistinguishable from corruption");
    return true;
}

#else
"""

OLD_STUB = """bool selftest_cosmofs_writeback(const char **reason) { (void)reason; return true; }
#endif
"""
NEW_STUB = """bool selftest_cosmofs_writeback(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_metadata_csum_id(const char **reason) { (void)reason; return true; }
#endif
"""

OLD_DECL = """bool selftest_cosmofs_csum(const char **reason);       /* data and directory checksums */
"""
NEW_DECL = """bool selftest_cosmofs_csum(const char **reason);       /* data and directory checksums */
bool selftest_cosmofs_metadata_csum_id(const char **reason);  /* the metadata header records no checksum algorithm */
"""

OLD_REG = """    { "cosmofs-csum",    selftest_cosmofs_csum },
"""
NEW_REG = """    { "cosmofs-csum",    selftest_cosmofs_csum },
    { "cosmofs-metadata-csum-id", selftest_cosmofs_metadata_csum_id },
"""

BACKUP = '.cosmofs-metadata-csum-probe.orig'
STAMP = '.cosmofs-metadata-csum-probe.applied'


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def write_atomic(path, data):
    # Whole or not at all: a probe interrupted mid-write must leave every
    # file either as it was or as intended, never half of each.
    tmp = path + '.probe-tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    if os.path.exists(path):   # keep the file's mode: the umask must not change it
        os.chmod(tmp, stat.S_IMODE(os.stat(path).st_mode))
    os.replace(tmp, path)


def git_clean(path):
    r = subprocess.run(['git', 'status', '--porcelain', '--', path], capture_output=True, text=True)
    if r.returncode != 0:     # a failed check is not a clean tree
        sys.exit(f'git status failed for {path}: {r.stderr.strip() or r.returncode}')
    return not r.stdout.strip()


def apply_files(fl):
    """Patch every file in `fl`, or none. Every patch is built in memory
    first; then the stamp is written, recording each file's original and
    patched hash; then each backup and each file is replaced atomically.
    From the stamp on, every file is exactly its original or its patched
    bytes, so revert can finish whatever an interruption left."""
    if os.path.exists(STAMP):
        sys.exit('already applied (or an apply was interrupted): run revert first')
    if os.path.exists(STAMP + '.probe-tmp'):
        os.remove(STAMP + '.probe-tmp')  # the stamp is written first: without it nothing was patched
    plan = []
    for path, edits in fl:
        if os.path.exists(path + BACKUP):
            sys.exit(f'{path + BACKUP} exists from an earlier run; restore or remove it by hand first')
        if not os.path.isfile(path):
            sys.exit(f'{path} not found: run from the top of the tree')
        if not git_clean(path):
            sys.exit(f'{path} has uncommitted changes')
        orig = open(path, 'rb').read()
        s = orig.decode()
        for a, b in edits:
            if s.count(a) != 1:
                sys.exit(f'{path}: anchor not found exactly once: {a[:50]!r}')
            s = s.replace(a, b)
        plan.append((path, orig, s.encode()))
    write_atomic(STAMP, ''.join(f'{p} {hashlib.sha256(n).hexdigest()} {hashlib.sha256(o).hexdigest()}\n'
                                for p, o, n in plan).encode())
    for path, orig, new in plan:
        write_atomic(path + BACKUP, orig)
        write_atomic(path, new)


def revert():
    if not os.path.exists(STAMP):
        if os.path.exists(STAMP + '.probe-tmp'):
            os.remove(STAMP + '.probe-tmp')   # an apply interrupted before its stamp: nothing was patched
            sys.exit('not applied (removed a partial stamp an interrupted apply left)')
        sys.exit('not applied')
    entries = [line.split() for line in open(STAMP).read().split('\n') if line]
    for path, patched, orig in entries:
        cur = sha(path)
        if cur == orig:
            continue                     # never patched, or already restored
        if cur != patched:
            sys.exit(f'{path} changed since apply. {path + BACKUP} is the pre-probe '
                     f'original, so copying it back would erase those changes. Keep your '
                     f'edits and remove the CFSPROBE lines by hand, then delete {path + BACKUP} '
                     f'and {STAMP}.')
        if not os.path.exists(path + BACKUP) or sha(path + BACKUP) != orig:
            sys.exit(f'{path} is still patched and {path + BACKUP} is missing or not its original; restore by hand')
    for path, patched, orig in entries:
        if sha(path) == patched:
            write_atomic(path, open(path + BACKUP, 'rb').read())
    for path, _, _ in entries:
        for leftover in (path + BACKUP, path + '.probe-tmp', path + BACKUP + '.probe-tmp'):
            if os.path.exists(leftover):
                os.remove(leftover)
    if os.path.exists(STAMP + '.probe-tmp'):
        os.remove(STAMP + '.probe-tmp')
    os.remove(STAMP)
    print('reverted')


def apply():
    apply_files([
        (TEST, [(OLD_TEST_TAIL, NEW_TEST_TAIL), (OLD_STUB, NEW_STUB)]),
        (DECL, [(OLD_DECL, NEW_DECL)]),
        (REG, [(OLD_REG, NEW_REG)]),
    ])
    print('applied: cosmofs-metadata-csum-id shows the metadata header records no checksum algorithm')


if __name__ == '__main__':
    {'apply': apply, 'revert': revert}.get(sys.argv[1] if len(sys.argv) > 1 else '',
                                           lambda: sys.exit(__doc__))()
