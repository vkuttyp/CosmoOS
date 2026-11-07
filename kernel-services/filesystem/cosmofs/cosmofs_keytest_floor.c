/*
 * cosmofs_keytest_floor.c - The C under cosmofs_keytest.cee (roadmap 2.0
 * SA7): the two parts of the cosmofs-encrypted-root self-test that a
 * build configuration decides -- the fault-injection build's copy of a
 * checksum index that cannot be had, and the debug build's pool of two
 * members whose label is forged (its format is a debug hook) -- and the
 * layout the CeeSafe half writes as its own constants, asserted here,
 * since a self-test has no host test to assert it. The test is CeeSafe
 * (cosmofs_keytest.cee, generated into cosmofs_keytest.c).
 */

#include <kernel/blk.h>
#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/faultinject.h>
#include <kernel/log.h>
#include <kernel/ramblk.h>
#include <kernel/thread.h>
#include <kernel/vfs.h>

#include <uapi/cosmo/fsctl.h>
#include <uapi/cosmo/keyslot.h>

#include "cosmofs_format.h"
#include "cosmofs_internal.h"

_Static_assert(CFS_BLOCK == 4096 && CFS_MHDR_SIZE == 32 && CFS_PTRS_PER_BLOCK == 508 && CFS_SUPER_A == 0 &&
                   CFS_SUPER_B == 1 && CFS_FORMAT_VERSION == 11 && CFS_KIND_CSUMIDX2 == 18 &&
                   offsetof(struct cfs_mhdr, kind) == 4 && offsetof(struct cfs_mhdr, crc) == 24 &&
                   offsetof(struct cfs_mhdr, csum_algo) == 28 && CFS_CSUM_CRC32C == 1 &&
                   offsetof(struct cfs_super, crc) == 160 && offsetof(struct cfs_super, free_blocks) == 64 &&
                   offsetof(struct cfs_member, free_blocks) == 40 && offsetof(struct cfs_member_block, m) == 8,
               "cosmofs_keytest.cee's cosmofs_format.h values and offsets");
_Static_assert(COSMO_KEY_SLOTS == 8 && sizeof(struct cosmo_keyslot) == 96 && COSMO_KEYSLOT_EMPTY == 0 &&
                   COSMO_KEYSLOT_PASSPHRASE == 1 && COSMO_KEYSLOT_KEYFILE == 2 && COSMOFS_KEY_INFO == 0 &&
                   COSMOFS_KEY_LOAD == 1 && COSMOFS_KEY_SLOT == 3 && COSMO_FSCTL_KEY_F_ENCRYPTED == 1 &&
                   COSMO_FSCTL_KEY_F_UNLOCKED == 2 && COSMO_FSCTL_KEY_F_MAC == 4 && COSMO_FSCTL_DEV_F_CHECKED == 1 &&
                   COSMO_FSCTL_DEV_F_SUPER_NONE == 2 && COSMO_FSCTL_DEV_F_MOUNT_FAILED == 8 &&
                   COSMO_FSCTL_DEV_F_FELL_BACK == 16 && COSMO_FSCTL_DEV_F_LOCKED == 2048 && COSMOFS_SCRUB_REPAIR == 1,
               "cosmofs_keytest.cee's keyslot.h, fsctl.h and cosmofs.h values");
_Static_assert(COSMO_O_RDONLY == 0 && COSMO_O_WRONLY == 1 && COSMO_O_RDWR == 2 && COSMO_O_CREAT == 0x40 &&
                   VFS_UMOUNT_FORCE == 1 && EEXIST == 17 && EINVAL == 22 && EIO == 5 && EBUSY == 16 &&
                   ENOKEY == 126 && EKEYREJECTED == 129 && ENOMEM == 12,
               "cosmofs_keytest.cee's vfs.h, syscall.h and errno.h values");

/* cosmofs_keytest.cee's: the test's mount and its helpers. */
int cfs_keytest_mount_dev(struct blkdev *bd);
struct mount *cfs_keytest_mount(void);
bool cfs_keytest_write_at(const char *path, uint64_t off, const void *data, size_t len);
bool cfs_keytest_read_at(const char *path, uint64_t off, const void *want, size_t len);
bool cfs_keytest_forge(struct blkdev *bd, uint64_t dva, size_t crc_off, bool header, size_t touch, uint8_t *save);
bool cfs_keytest_put_back(struct blkdev *bd, uint64_t dva, const uint8_t *save);
int cfs_keytest_load(struct blkdev *bd, const uint8_t *key);

bool cfs_keytest_floor_cow(const uint8_t *blk, const char **reason);
bool cfs_keytest_floor_labels(struct blkdev **two, const struct cosmo_keyslot *slots, const uint8_t *uuid2,
                              const uint8_t *master2, uint8_t *save, const char **reason);
void cfs_keytest_floor_clear(void);

#define KT "/mnt/keytest"
#define KT_FAR_LBLK 70000u
#define STR_(x) #x
#define STR(x)  STR_(x)
#define KT_CHECK(cond)                                                         \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

/* The far block rewritten when its mid-level index cannot be copied: the
 * write-back is -ENOMEM and the retry commits. The failed copy used to
 * leave the walk holding the block it had released, and its cleanup
 * released it again (Codex on #447). Nothing without fault injection. */
bool cfs_keytest_floor_cow(const uint8_t *blk, const char **reason)
{
#if CONFIG_FAULTINJECT
    struct file *f;
    KT_CHECK(vfs_open(NULL, KT "/far", COSMO_O_RDWR, 0, &f) == 0);
    bool w = file_pwrite(f, blk, CFS_BLOCK, (uint64_t)KT_FAR_LBLK * CFS_BLOCK) == CFS_BLOCK;
    faultinject_set(FI_COSMOFS_CSUM_COW, 1, 1, thread_current());
    int first = w ? file_sync(f) : -1;
    struct fi_stats fst;
    faultinject_stats(FI_COSMOFS_CSUM_COW, &fst);
    faultinject_clear(FI_COSMOFS_CSUM_COW);
    int retry = file_sync(f);
    file_put(f);
    kinfo("selftest: cosmofs-encrypted-root: a two-level index's mid block not copied: sync %d (hits %llu), "
          "retry %d", first, (unsigned long long)fst.hits, retry);
    KT_CHECK(w && fst.hits == 1 && first == -ENOMEM && retry == 0);
#else
    (void)blk;
    (void)reason;
#endif
    return true;
}

/* The labels: a pool of two members, the second's label forged (the
 * format of two members is a test hook, in a debug build only). */
bool cfs_keytest_floor_labels(struct blkdev **two, const struct cosmo_keyslot *slots, const uint8_t *uuid2,
                              const uint8_t *master2, uint8_t *save, const char **reason)
{
#if CONFIG_DEBUG
    static const char secret[] = "an encrypted root keeps this sentence off the disk";
    two[0] = ramblk_create(1024);
    two[1] = ramblk_create(1024);
    KT_CHECK(two[0] != NULL && two[1] != NULL);
    KT_CHECK(cosmofs_test_format_keyed_members(two, 2, uuid2, master2, slots) == 0);
    KT_CHECK(cfs_keytest_mount_dev(two[0]) == 0);
    bool w = cfs_keytest_write_at(KT "/x", 0, secret, sizeof(secret) - 1);
    (void)vfs_umount(KT);
    KT_CHECK(w);
    KT_CHECK(cfs_keytest_load(two[0], master2) == 0);
    KT_CHECK(cfs_keytest_forge(two[1], 0, offsetof(struct cfs_label, crc), false,
                               offsetof(struct cfs_label, nblocks) + 7, save));
    int lrc = cfs_keytest_mount_dev(two[0]);
    if (lrc == 0)
        (void)vfs_umount(KT);
    KT_CHECK(lrc != 0);
    KT_CHECK(cfs_keytest_put_back(two[1], 0, save));
    KT_CHECK(cfs_keytest_mount_dev(two[0]) == 0);
    bool r = cfs_keytest_read_at(KT "/x", 0, secret, sizeof(secret) - 1);
    (void)vfs_umount(KT);
    KT_CHECK(r);
    kinfo("selftest: cosmofs-encrypted-root: member 1's label forged with its CRC32C recomputed: mount refused (%d)",
          lrc);
#else
    (void)two;
    (void)slots;
    (void)uuid2;
    (void)master2;
    (void)save;
    (void)reason;
#endif
    return true;
}

void cfs_keytest_floor_clear(void)
{
#if CONFIG_FAULTINJECT
    faultinject_clear(FI_COSMOFS_CSUM_COW);
#endif
}
