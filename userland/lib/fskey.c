/* fskey.c - an encrypted pool's key through /dev/fsctl (fskey.h). */
#include "fskey.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--)
        *v++ = 0;
}

/* Write `k` whole and read back the result into `out` (may be NULL). */
static int run(struct cosmo_fsctl_key *k, const char *dev, uint64_t mount_id, void *out, size_t outlen)
{
    k->cmd.version = COSMO_FSCTL_VERSION;
    k->cmd.mount_id = dev ? 0 : mount_id;
    if (dev) {
        if (strlen(dev) >= sizeof(k->cmd.name)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(k->cmd.name, dev);
    }
    int fd = open("/dev/fsctl", O_RDWR);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, k, sizeof(*k));
    int err = errno;
    wipe(k, sizeof(*k));
    if (n != (ssize_t)sizeof(*k)) {
        close(fd);
        errno = n < 0 ? err : EIO;
        return -1;
    }
    int rc = 0;
    if (out != NULL) {
        uint8_t buf[sizeof(struct cosmo_fsctl_result) + sizeof(struct cosmo_fsctl_keyinfo)];
        n = read(fd, buf, sizeof(buf));
        const struct cosmo_fsctl_result *h = (const struct cosmo_fsctl_result *)buf;
        if (n < (ssize_t)sizeof(*h) || h->count != 1 || h->bytes != outlen ||
            (size_t)n < sizeof(*h) + outlen) {
            errno = EIO;
            rc = -1;
        } else {
            memcpy(out, buf + sizeof(*h), outlen);
        }
    }
    close(fd);
    return rc;
}

int fskey_info(const char *dev, uint64_t mount_id, struct cosmo_fsctl_keyinfo *out)
{
    struct cosmo_fsctl_key k;
    memset(&k, 0, sizeof(k));
    k.cmd.op = COSMO_FSCTL_KEY_INFO;
    return run(&k, dev, mount_id, out, sizeof(*out));
}

int fskey_load(const char *dev, const uint8_t master[COSMO_KEY_SIZE])
{
    struct cosmo_fsctl_key k;
    memset(&k, 0, sizeof(k));
    k.cmd.op = COSMO_FSCTL_KEY_LOAD;
    memcpy(k.key, master, COSMO_KEY_SIZE);
    return run(&k, dev, 0, NULL, 0);
}

int fskey_unload(const char *dev)
{
    struct cosmo_fsctl_key k;
    memset(&k, 0, sizeof(k));
    k.cmd.op = COSMO_FSCTL_KEY_UNLOAD;
    return run(&k, dev, 0, NULL, 0);
}

int fskey_slot(uint64_t mount_id, unsigned slot, const uint8_t master[COSMO_KEY_SIZE], const struct cosmo_keyslot *rec)
{
    struct cosmo_fsctl_key k;
    memset(&k, 0, sizeof(k));
    k.cmd.op = COSMO_FSCTL_KEY_SLOT;
    k.slot = slot;
    memcpy(k.key, master, COSMO_KEY_SIZE);
    k.rec = *rec;
    return run(&k, NULL, mount_id, NULL, 0);
}
