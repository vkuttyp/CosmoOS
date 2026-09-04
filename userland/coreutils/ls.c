/* ls - list directory contents. -l long form, -a include dot entries. */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int opt_long, opt_all;

static char type_char(unsigned t)
{
    switch (t) {
    case DT_DIR: return 'd';
    case DT_CHR: return 'c';
    case DT_FIFO: return 'p';
    case DT_SOCK: return 's';
    default: return '-';
    }
}

static void print_long(const char *dir, const char *name, unsigned type)
{
    char path[1024];
    struct stat st;
    if (dir)
        snprintf(path, sizeof(path), "%s/%s", dir, name);
    else
        snprintf(path, sizeof(path), "%s", name);
    if (stat(path, &st) < 0) {
        printf("?--------- %8s %s\n", "?", name);
        return;
    }
    if (type == DT_UNKNOWN)
        type = st.st_type;
    char mode[10];
    for (int i = 0; i < 9; i++)
        mode[i] = (st.st_mode & (0400u >> i)) ? "rwxrwxrwx"[i] : '-';
    mode[9] = '\0';
    printf("%c%s %3u %4u %4u %8llu %6llu %s\n", type_char(type), mode, st.st_nlink, st.st_uid, st.st_gid,
           (unsigned long long)st.st_size, (unsigned long long)st.st_ino, name);
}

struct entry {
    char name[256];
    unsigned type;
};

static int cmp_entry(const void *a, const void *b)
{
    return strcmp(((const struct entry *)a)->name, ((const struct entry *)b)->name);
}

static int list_dir(const char *path, int show_header)
{
    DIR *d = opendir(path);
    if (d == NULL) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    size_t n = 0, cap = 64;
    struct entry *ents = malloc(cap * sizeof(*ents));
    struct dirent *e;
    while (ents && (e = readdir(d)) != NULL) {
        if (!opt_all && e->d_name[0] == '.')
            continue;
        if (n == cap) {
            cap *= 2;
            struct entry *ne = realloc(ents, cap * sizeof(*ents));
            if (ne == NULL)
                break;
            ents = ne;
        }
        strlcpy(ents[n].name, e->d_name, sizeof(ents[n].name));
        ents[n].type = e->d_type;
        n++;
    }
    closedir(d);
    if (ents == NULL) {
        fprintf(stderr, "ls: out of memory\n");
        return 1;
    }
    qsort(ents, n, sizeof(*ents), cmp_entry);
    if (show_header)
        printf("%s:\n", path);
    for (size_t i = 0; i < n; i++) {
        if (opt_long)
            print_long(path, ents[i].name, ents[i].type);
        else
            printf("%s\n", ents[i].name);
    }
    free(ents);
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'l')
                opt_long = 1;
            else if (*o == 'a')
                opt_all = 1;
            else {
                fprintf(stderr, "ls: unknown option -%c\n", *o);
                return 2;
            }
        }
    }
    if (i == argc)
        return list_dir(".", 0);
    int rc = 0, many = argc - i > 1;
    for (int k = i; k < argc; k++) {
        struct stat st;
        if (stat(argv[k], &st) < 0) {
            fprintf(stderr, "ls: %s: %s\n", argv[k], strerror(errno));
            rc = 1;
            continue;
        }
        if (S_ISDIR(st.st_type)) {
            if (list_dir(argv[k], many))
                rc = 1;
        } else if (opt_long) {
            print_long(NULL, argv[k], st.st_type);
        } else {
            printf("%s\n", argv[k]);
        }
    }
    return rc;
}
