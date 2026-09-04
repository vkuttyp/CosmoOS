/* cp - copy files. -r copies directories recursively. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char buf[16384];
static int opt_recursive;

static int copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "cp: %s: %s\n", src, strerror(errno));
        return 1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        fprintf(stderr, "cp: %s: %s\n", dst, strerror(errno));
        close(in);
        return 1;
    }
    int rc = 0;
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n < 0) {
            fprintf(stderr, "cp: %s: %s\n", src, strerror(errno));
            rc = 1;
            break;
        }
        if (n == 0)
            break;
        ssize_t done = 0;
        while (done < n) {
            ssize_t w = write(out, buf + done, (size_t)(n - done));
            if (w <= 0) {
                fprintf(stderr, "cp: %s: %s\n", dst, strerror(errno));
                rc = 1;
                break;
            }
            done += w;
        }
        if (rc)
            break;
    }
    close(in);
    close(out);
    return rc;
}

static int copy_any(const char *src, const char *dst);

static int copy_dir(const char *src, const char *dst)
{
    if (mkdir(dst, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "cp: %s: %s\n", dst, strerror(errno));
        return 1;
    }
    DIR *d = opendir(src);
    if (d == NULL) {
        fprintf(stderr, "cp: %s: %s\n", src, strerror(errno));
        return 1;
    }
    int rc = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char s[1024], t[1024];
        snprintf(s, sizeof(s), "%s/%s", src, e->d_name);
        snprintf(t, sizeof(t), "%s/%s", dst, e->d_name);
        if (copy_any(s, t))
            rc = 1;
    }
    closedir(d);
    return rc;
}

static int copy_any(const char *src, const char *dst)
{
    struct stat st;
    if (stat(src, &st) < 0) {
        fprintf(stderr, "cp: %s: %s\n", src, strerror(errno));
        return 1;
    }
    if (S_ISDIR(st.st_type)) {
        if (!opt_recursive) {
            fprintf(stderr, "cp: %s: is a directory (use -r)\n", src);
            return 1;
        }
        return copy_dir(src, dst);
    }
    struct stat dst_st;
    if (stat(dst, &dst_st) == 0 && S_ISDIR(dst_st.st_type)) {
        const char *base = strrchr(src, '/');
        base = base ? base + 1 : src;
        char t[1024];
        snprintf(t, sizeof(t), "%s/%s", dst, base);
        return copy_file(src, t);
    }
    return copy_file(src, dst);
}

int main(int argc, char **argv)
{
    int i = 1;
    if (i < argc && strcmp(argv[i], "-r") == 0) {
        opt_recursive = 1;
        i++;
    }
    if (argc - i < 2) {
        fprintf(stderr, "usage: cp [-r] source... target\n");
        return 2;
    }
    const char *dst = argv[argc - 1];
    int rc = 0;
    for (; i < argc - 1; i++)
        if (copy_any(argv[i], dst))
            rc = 1;
    return rc;
}
