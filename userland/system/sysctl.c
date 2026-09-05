/* sysctl - read kernel values: sysctl -a | sysctl name... */
#include <cosmo/sysctl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int show(const char *name)
{
    char value[1024];
    if (sysctl_get(name, value, sizeof(value)) < 0) {
        fprintf(stderr, "sysctl: %s: %s\n", name, strerror(errno));
        return 1;
    }
    printf("%s = %s\n", name, value);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "-a") == 0) {
        char names[2048];
        if (sysctl_get("sysctl.names", names, sizeof(names)) < 0) {
            perror("sysctl");
            return 1;
        }
        int rc = 0;
        char *save;
        for (char *n = strtok_r(names, "\n", &save); n; n = strtok_r(NULL, "\n", &save))
            if (strcmp(n, "sysctl.names") != 0 && show(n))
                rc = 1;
        return rc;
    }
    if (argc < 2) {
        fprintf(stderr, "usage: sysctl -a | sysctl name...\n");
        return 2;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++)
        if (show(argv[i]))
            rc = 1;
    return rc;
}
