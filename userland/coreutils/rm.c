/* rm - remove files. -r removes directories and their contents. */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int opt_recursive;

static int remove_any(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        fprintf(stderr, "rm: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (S_ISDIR(st.st_type)) {
        if (!opt_recursive) {
            fprintf(stderr, "rm: %s: is a directory (use -r)\n", path);
            return 1;
        }
        DIR *d = opendir(path);
        if (d == NULL) {
            fprintf(stderr, "rm: %s: %s\n", path, strerror(errno));
            return 1;
        }
        int rc = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            char child[1024];
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            if (remove_any(child))
                rc = 1;
        }
        closedir(d);
        if (rc)
            return rc;
        if (rmdir(path) < 0) {
            fprintf(stderr, "rm: %s: %s\n", path, strerror(errno));
            return 1;
        }
        return 0;
    }
    if (unlink(path) < 0) {
        fprintf(stderr, "rm: %s: %s\n", path, strerror(errno));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1;
    if (i < argc && (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "-rf") == 0)) {
        opt_recursive = 1;
        i++;
    }
    if (i == argc) {
        fprintf(stderr, "usage: rm [-r] path...\n");
        return 2;
    }
    int rc = 0;
    for (; i < argc; i++)
        if (remove_any(argv[i]))
            rc = 1;
    return rc;
}
