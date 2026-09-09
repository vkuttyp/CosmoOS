/* termios.c - The terminal's modes (docs/libc/design.md). */

#include <errno.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "libc.h"

int tcgetattr(int fd, struct termios *t)
{
    struct cosmo_termios k;
    long rc = cosmo_tcgetattr(fd, &k);
    if (rc < 0)
        return (int)__syscall_ret(rc);
    memset(t, 0, sizeof(*t));
    t->c_iflag = k.modes & ICRNL;
    t->c_lflag = k.modes & (ECHO | ICANON | ISIG);
    t->c_cc[VMIN] = k.vmin;
    t->c_cc[VTIME] = k.vtime;
    return 0;
}

int tcsetattr(int fd, int actions, const struct termios *t)
{
    (void)actions;   /* see termios.h: the three are alike here */
    struct cosmo_termios k;
    memset(&k, 0, sizeof(k));
    k.modes = (t->c_iflag & ICRNL) | (t->c_lflag & (ECHO | ICANON | ISIG));
    k.vmin = t->c_cc[VMIN];
    k.vtime = t->c_cc[VTIME];
    return (int)__syscall_ret(cosmo_tcsetattr(fd, &k));
}

void cfmakeraw(struct termios *t)
{
    t->c_iflag &= ~(tcflag_t)ICRNL;
    t->c_lflag &= ~(tcflag_t)(ECHO | ICANON | ISIG);
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}

int tcgetwinsize(int fd, struct winsize *ws)
{
    struct cosmo_ttysize sz;
    long rc = cosmo_ttysize(fd, &sz);
    if (rc < 0)
        return (int)__syscall_ret(rc);
    ws->ws_col = sz.cols;
    ws->ws_row = sz.rows;
    return 0;
}
