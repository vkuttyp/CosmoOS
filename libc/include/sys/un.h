#ifndef _SYS_UN_H
#define _SYS_UN_H

#include <sys/types.h>
#include <uapi/cosmo/syscall.h>

/* A unix socket's address: the family and a NUL-terminated path, or a
 * leading NUL and the bytes of an abstract name, the length passed to
 * bind/connect/sendto delimiting it (2 + the name's bytes). The same
 * layout as struct cosmo_sockaddr_un. */
struct sockaddr_un {
    uint16_t sun_family;
    char sun_path[COSMO_UNIX_PATH_MAX];
};

#endif
