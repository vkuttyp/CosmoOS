/* mkfifo - make named pipes (mode 0644; the shell's `a > fifo & cat fifo`). */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: mkfifo name...\n");
        return 2;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (mkfifo(argv[i], 0644) < 0) {
            fprintf(stderr, "mkfifo: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
