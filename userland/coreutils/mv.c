/* mv - rename; across mounts, copy and unlink (files only). */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char buf[16384];

static int copy_unlink(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        close(in);
        return -1;
    }
    int rc = 0;
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n < 0) {
            rc = -1;
            break;
        }
        if (n == 0)
            break;
        if (write(out, buf, (size_t)n) != n) {
            rc = -1;
            break;
        }
    }
    close(in);
    close(out);
    if (rc == 0)
        rc = unlink(src);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: mv source target\n");
        return 2;
    }
    char target[1024];
    const char *dst = argv[2];
    struct stat st;
    if (stat(dst, &st) == 0 && S_ISDIR(st.st_type)) {
        const char *base = strrchr(argv[1], '/');
        base = base ? base + 1 : argv[1];
        snprintf(target, sizeof(target), "%s/%s", dst, base);
        dst = target;
    }
    if (rename(argv[1], dst) == 0)
        return 0;
    if (errno == EXDEV && copy_unlink(argv[1], dst) == 0)
        return 0;
    fprintf(stderr, "mv: %s -> %s: %s\n", argv[1], dst, strerror(errno));
    return 1;
}
