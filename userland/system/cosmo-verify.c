/*
 * cosmo-verify - check a release file against the release's signed
 * SHA256SUMS (docs/guide/upgrading.md; docs/userland/design.md, "Updating").
 *
 *   cosmo-verify FILE SUMS SIG
 *
 * SIG is SUMS's detached signature (SHA256SUMS.sig: the COSMOSIG trailer
 * on its own), checked against the keys in /etc/pkg/keys, pkg's and
 * cosmo-upgrade's trust root. FILE's SHA-256 is then compared with the line
 * SUMS gives its base name. Exit 0 when both hold, 1 when a file cannot be
 * read, 2 usage, 3 refused.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "relsig.h"

enum { EXIT_OK = 0, EXIT_FAILED = 1, EXIT_USAGE = 2, EXIT_REFUSED = 3 };

static int read_whole(const char *path, uint8_t **out, size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -errno;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_type)) {
        close(fd);
        return -EINVAL;
    }
    uint8_t *buf = malloc(st.st_size ? (size_t)st.st_size : 1);
    if (buf == NULL) {
        close(fd);
        return -ENOMEM;
    }
    size_t done = 0;
    while (done < st.st_size) {
        ssize_t n = read(fd, buf + done, (size_t)st.st_size - done);
        if (n <= 0) {
            int e = n < 0 ? errno : EIO;
            close(fd);
            free(buf);
            return -e;
        }
        done += (size_t)n;
    }
    close(fd);
    *out = buf;
    *len = done;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: cosmo-verify FILE SUMS SIG\n");
        return EXIT_USAGE;
    }
    const char *paths[3] = { argv[1], argv[2], argv[3] };
    uint8_t *data[3] = { NULL, NULL, NULL };
    size_t len[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; i++) {
        int rc = read_whole(paths[i], &data[i], &len[i]);
        if (rc < 0) {
            fprintf(stderr, "cosmo-verify: %s: %s\n", paths[i], strerror(-rc));
            return EXIT_FAILED;
        }
    }
    relsig_ring_load(RELSIG_KEYS_DIR);
    char keyid[17], err[200];
    if (relsig_sums_verify(data[1], len[1], data[2], len[2], keyid, err, sizeof(err)) < 0) {
        fprintf(stderr, "cosmo-verify: %s: %s; do not use %s\n", paths[1], err, paths[0]);
        return EXIT_REFUSED;
    }
    const char *name = strrchr(paths[0], '/');
    name = name ? name + 1 : paths[0];
    uint8_t want[RELSIG_SHA256_SIZE], got[RELSIG_SHA256_SIZE];
    if (relsig_sums_lookup(data[1], len[1], name, want, err, sizeof(err)) < 0) {
        fprintf(stderr, "cosmo-verify: %s; do not use %s\n", err, paths[0]);
        return EXIT_REFUSED;
    }
    relsig_sha256(data[0], len[0], got);
    if (memcmp(got, want, sizeof(got)) != 0) {
        fprintf(stderr, "cosmo-verify: %s does not match SHA256SUMS; do not use it\n", paths[0]);
        return EXIT_REFUSED;
    }
    char hex[2 * RELSIG_SHA256_SIZE + 1];
    relsig_hex(got, sizeof(got), hex);
    const struct relsig_key *k = relsig_key_find(data[2] + 64);
    printf("cosmo-verify: %s: OK (SHA-256 %.16s..., in SHA256SUMS signed by key %s, %s in %s)\n", paths[0], hex, keyid,
           k->name, RELSIG_KEYS_DIR);
    for (int i = 0; i < 3; i++)
        free(data[i]);
    return EXIT_OK;
}
