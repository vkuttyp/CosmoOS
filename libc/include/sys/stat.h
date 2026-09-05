#ifndef _SYS_STAT_H
#define _SYS_STAT_H
#include <sys/types.h>
#include <uapi/cosmo/syscall.h>
/* The native stat record is the ABI; the st_* names are views on it. */
struct stat {
    uint64_t st_ino;
    uint32_t st_type;      /* DT_* */
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t st_pad;
    uint64_t st_size;
    uint64_t st_mtime_ns;
    uint64_t st_ctime_ns;
};
#define S_ISREG(st_type_of) ((st_type_of) == COSMO_DT_REG)
#define S_ISDIR(st_type_of) ((st_type_of) == COSMO_DT_DIR)
#define S_ISCHR(st_type_of) ((st_type_of) == COSMO_DT_CHR)
#define S_ISFIFO(st_type_of) ((st_type_of) == COSMO_DT_FIFO)
#define S_ISSOCK(st_type_of) ((st_type_of) == COSMO_DT_SOCK)
int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int mkdir(const char *path, mode_t mode);
#endif
