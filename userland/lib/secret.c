/* secret.c - a passphrase from the standard input (secret.h). */
#include "secret.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

void secret_wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--)
        *v++ = 0;
}

int secret_read(const char *prompt, char *buf, size_t n)
{
    /* Echo off before the prompt: a reply sent the moment the prompt is
     * seen (a paste, the boot harness) is not echoed. */
    struct termios old, quiet;
    bool tty = tcgetattr(0, &old) == 0;
    if (tty) {
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(0, TCSANOW, &quiet);
    }
    fputs(prompt, stdout);
    fflush(stdout);
    /* A byte at a time: what follows the line is the next reader's. */
    size_t len = 0;
    int got = -1;
    for (;;) {
        char c;
        if (read(0, &c, 1) <= 0)
            break;
        if (c == '\n' || c == '\r') {
            got = (int)len;
            break;
        }
        if (len + 1 < n)
            buf[len++] = c;
    }
    buf[len] = '\0';
    if (tty) {
        tcsetattr(0, TCSANOW, &old);
        fputs("\n", stdout);   /* the newline the terminal did not echo */
        fflush(stdout);
    }
    return got;
}

int secret_read_new(const char *prog, char *buf, size_t n)
{
    char again[SECRET_MAX];
    int len = secret_read("new passphrase: ", buf, n);
    int len2 = len < 0 ? -1 : secret_read("new passphrase again: ", again, sizeof(again));
    int rc = len;
    if (len < 0 || len2 < 0) {
        fprintf(stderr, "%s: no passphrase (end of input)\n", prog);
        rc = -1;
    } else if (len == 0) {
        fprintf(stderr, "%s: an empty passphrase is not one\n", prog);
        rc = -1;
    } else if (len2 != len || strcmp(buf, again) != 0) {
        fprintf(stderr, "%s: the two passphrases differ\n", prog);
        rc = -1;
    }
    secret_wipe(again, sizeof(again));
    return rc;
}
