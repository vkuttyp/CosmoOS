/* mount - attach a filesystem: mount [-r] source target fstype */
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>

int main(int argc, char **argv)
{
    int i = 1;
    unsigned flags = 0;
    if (i < argc && strcmp(argv[i], "-r") == 0) {
        flags |= MS_RDONLY;
        i++;
    }
    if (argc - i != 3) {
        fprintf(stderr, "usage: mount [-r] source target fstype\n");
        return 2;
    }
    if (mount(argv[i], argv[i + 1], argv[i + 2], flags) < 0) {
        perror("mount");
        return 1;
    }
    return 0;
}
