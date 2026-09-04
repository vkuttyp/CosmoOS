#ifndef _DIRENT_H
#define _DIRENT_H
#include <sys/types.h>
#include <uapi/cosmo/syscall.h>
#define DT_UNKNOWN COSMO_DT_UNKNOWN
#define DT_REG COSMO_DT_REG
#define DT_DIR COSMO_DT_DIR
#define DT_CHR COSMO_DT_CHR
#define DT_FIFO COSMO_DT_FIFO
#define DT_SOCK COSMO_DT_SOCK
struct dirent {
    ino_t d_ino;
    unsigned char d_type;
    char d_name[256];
};
typedef struct _DIR DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);
#endif
