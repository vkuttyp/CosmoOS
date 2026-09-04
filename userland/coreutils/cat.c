/* cat - concatenate files (standard input without arguments or for "-"). */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char buf[16384];

static int copy(int fd)
{
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0)
            return -1;
        if (n == 0)
            return 0;
        ssize_t done = 0;
        while (done < n) {
            ssize_t w = write(1, buf + done, (size_t)(n - done));
            if (w <= 0)
                return -1;
            done += w;
        }
    }
}

int main(int argc, char **argv)
{
    int rc = 0;
    if (argc < 2) {
        if (copy(0) < 0) {
            perror("cat: stdin");
            return 1;
        }
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        int fd = strcmp(argv[i], "-") == 0 ? 0 : open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        if (copy(fd) < 0) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
        if (fd != 0)
            close(fd);
    }
    return rc;
}
