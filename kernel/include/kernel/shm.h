/* shm.h - System V shared-memory segment registry.
 *
 * A segment is an anonymous ramfs regular file (the memfd backing) with a
 * System V key/id namespace layered on top. The segment RECORD is reference
 * counted: the registry holds one reference while the segment is not removed,
 * and each live attach holds one. IPC_RMID drops the registry reference and
 * unlinks the segment from the id/key tables; the record (and its backing) is
 * freed on the last reference -- i.e. the last detach after removal -- which is
 * exactly SysV's "destroyed on the last detach". See
 * docs/audit/next-subsystem-shm.md. Used only by the Linux personality.
 */
#ifndef KERNEL_SHM_H
#define KERNEL_SHM_H

#include <kernel/types.h>

struct shm_segment;
struct vnode;

/* What IPC_STAT reports about a segment. */
struct shm_stat {
    int32_t  key;
    size_t   size;
    uint32_t mode;
    uint32_t cuid, cgid;
    uint32_t nattch;
};

/* shmget: find-or-create a segment. `key` 0 (IPC_PRIVATE) always makes a new
 * keyless one. For a real key: an existing segment is returned (its id), unless
 * IPC_EXCL is set with IPC_CREAT (-EEXIST); a missing one is created only with
 * IPC_CREAT (else -ENOENT). `shmflg`'s low nine bits are the mode. Returns the
 * shmid (>= 0) or -errno. On create, `size` must be non-zero and within the
 * user window. */
int shm_get(int32_t key, size_t size, unsigned shmflg, uint32_t uid, uint32_t gid);

/* Look up a live segment by id and take a reference on its record (held by the
 * attach it is about to become). 0 and *out, or -EINVAL if no such live id.
 * On a failed attach the caller returns the reference with shm_unref. */
int shm_lookup_ref(int shmid, struct shm_segment **out);

/* The backing vnode and size of a segment the caller holds a reference to. */
struct vnode *shm_vnode(struct shm_segment *seg);
size_t shm_size(struct shm_segment *seg);

/* A successful attach bumps the segment's live-attach count; the reference
 * taken by shm_lookup_ref becomes the attach's. */
void shm_attached(struct shm_segment *seg);

/* Detach: drop the attach count and the attach's reference (freeing the record
 * and its backing if it was the last reference and the segment was removed). */
void shm_detach(struct shm_segment *seg);

/* Return a reference taken by shm_lookup_ref without having attached (an attach
 * that failed after the lookup). */
void shm_unref(struct shm_segment *seg);

/* IPC_STAT: fill *out for a live segment. 0 or -EINVAL. */
int shm_stat_id(int shmid, struct shm_stat *out);

/* IPC_RMID: unlink the segment from the id/key tables and drop the registry
 * reference; live attaches keep it alive until they detach. 0 or -EINVAL. */
int shm_rmid(int shmid);

#endif /* KERNEL_SHM_H */
