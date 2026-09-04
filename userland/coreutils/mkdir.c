/* mkdir - create directories. -p creates parents and tolerates existing ones. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int mkdir_p(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(path, 0755) < 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1, parents = 0;
    if (i < argc && strcmp(argv[i], "-p") == 0) {
        parents = 1;
        i++;
    }
    if (i == argc) {
        fprintf(stderr, "usage: mkdir [-p] directory...\n");
        return 2;
    }
    int rc = 0;
    for (; i < argc; i++) {
        int r = parents ? mkdir_p(argv[i]) : mkdir(argv[i], 0755);
        if (r < 0) {
            fprintf(stderr, "mkdir: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
