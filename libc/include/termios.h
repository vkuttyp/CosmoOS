/*
 * termios.h - The terminal's modes (docs/libc/design.md, "Terminals").
 *
 * This is the POSIX face of `struct cosmo_termios`, which carries the
 * four flags and two numbers this kernel's line discipline actually
 * has. POSIX's own structure describes a serial line -- baud rates,
 * parity, forty flags -- that this tree does not model, so the fields
 * that would be lies are simply absent rather than present and ignored.
 * A program that needs them is a program this libc cannot serve yet,
 * and it should fail to compile rather than silently misbehave.
 */
#ifndef _TERMIOS_H
#define _TERMIOS_H
#include <sys/types.h>
#include <uapi/cosmo/syscall.h>

typedef unsigned tcflag_t;

/* c_lflag */
#define ECHO   COSMO_TTY_ECHO
#define ICANON COSMO_TTY_ICANON
#define ISIG   COSMO_TTY_ISIG
/* c_iflag */
#define ICRNL  COSMO_TTY_ICRNL

#define VMIN  0
#define VTIME 1
#define NCCS  2

struct termios {
    tcflag_t c_iflag;   /* ICRNL */
    tcflag_t c_lflag;   /* ECHO, ICANON, ISIG */
    unsigned char c_cc[NCCS];
};

/* tcsetattr's `actions`: all three behave alike here, because the
 * kernel drops queued input exactly when the canonical bit changes and
 * has no output queue to drain. */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int actions, const struct termios *t);
/* Modes for reading a keystroke at a time: no echo, no line editing, no
 * signals from the keyboard, one byte per read. */
void cfmakeraw(struct termios *t);

struct winsize {
    unsigned short ws_col, ws_row;
};
/* The terminal's size, 0x0 when it does not know (a serial line). */
int tcgetwinsize(int fd, struct winsize *ws);

#endif
