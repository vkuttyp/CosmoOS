/*
 * shmprobe.c - SysV shared memory under operation-specific privileges
 * (roadmap M5; tests/security/priv.sh). A static musl program:
 *
 *   shmprobe create KEY      a 0600 segment of one page; prints its id
 *   shmprobe attach ID       shmat; prints "attach=ok" or "attach=<errno name>"
 *   shmprobe remove ID       IPC_RMID; prints "remove=ok" or "remove=<errno name>"
 *
 * The permission override for a segment one does not own is PRIV_SYS_ADMIN,
 * as for files: uid 0 without it is refused like anyone else.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>

static const char *name(int e)
{
    return e == EACCES ? "EACCES" : e == EPERM ? "EPERM" : e == EINVAL ? "EINVAL" : "other";
}

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    if (strcmp(argv[1], "create") == 0) {
        int id = shmget((key_t)strtol(argv[2], NULL, 0), 4096, IPC_CREAT | IPC_EXCL | 0600);
        if (id < 0) {
            printf("create=%s\n", name(errno));
            return 1;
        }
        printf("%d\n", id);
        return 0;
    }
    int id = atoi(argv[2]);
    if (strcmp(argv[1], "attach") == 0) {
        void *p = shmat(id, NULL, 0);
        printf("attach=%s\n", p == (void *)-1 ? name(errno) : "ok");
        return p == (void *)-1;
    }
    if (strcmp(argv[1], "remove") == 0) {
        int rc = shmctl(id, IPC_RMID, NULL);
        printf("remove=%s\n", rc < 0 ? name(errno) : "ok");
        return rc < 0;
    }
    return 2;
}
