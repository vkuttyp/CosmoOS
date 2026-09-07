/*
 * lxrights - a handle is a capability whatever ABI asks about it.
 *
 * The parent hands this program a socket on fd 3 with some rights
 * removed and names them in argv[1]; each named operation must be
 * refused here, through the *Linux* system calls, exactly as it is
 * through the native ones (docs/kernel/object/architecture.md, "The
 * upper sixteen bits"). Without this the restriction would last only
 * until the holder asked in the other language.
 *
 * Exit 0 if every named operation was refused with EPERM, else a code
 * saying which one was not.
 */
#include "lxabi.h"

/* Linux and CosmoOS agree on this one. */
#define LX_EPERM 1

#define SOCKFD 3

struct sa_in {
    unsigned short family;
    unsigned short port;
    unsigned int addr;
    unsigned char pad[8];
};

static int has(const char *list, char c)
{
    for (const char *p = list; *p; p++)
        if (*p == c)
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 90;
    const char *dropped = argv[1];
    struct sa_in sa = { 2, 0x7017 /* 5999, network order */, 0x0100007f, { 0 } };

    if (has(dropped, 'b')) {
        if (sc3(LX_bind, SOCKFD, &sa, sizeof(sa)) != -LX_EPERM)
            return 1;
        if (sc2(LX_listen, SOCKFD, 4) != -LX_EPERM)
            return 2;
    }
    if (has(dropped, 'c')) {
        if (sc3(LX_connect, SOCKFD, &sa, sizeof(sa)) != -LX_EPERM)
            return 3;
    }
    if (has(dropped, 'a')) {
        if (sc3(LX_accept, SOCKFD, 0, 0) != -LX_EPERM)
            return 4;
    }
    if (has(dropped, 's')) {
        if (sc2(LX_shutdown, SOCKFD, 2) != -LX_EPERM)
            return 5;
    }
    lx_puts("lxrights ok\n");
    return 0;
}
