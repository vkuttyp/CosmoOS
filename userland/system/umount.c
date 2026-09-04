/* umount - detach a filesystem: umount [-f] target */
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>

int main(int argc, char **argv)
{
    int i = 1;
    unsigned flags = 0;
    if (i < argc && strcmp(argv[i], "-f") == 0) {
        flags |= MNT_FORCE;
        i++;
    }
    if (argc - i != 1) {
        fprintf(stderr, "usage: umount [-f] target\n");
        return 2;
    }
    if (umount2(argv[i], flags) < 0) {
        perror("umount");
        return 1;
    }
    return 0;
}
