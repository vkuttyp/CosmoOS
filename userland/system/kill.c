/* kill - terminate processes: kill [-sig | -s sig] pid... */
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_sig(const char *s)
{
    if (isdigit((unsigned char)s[0]))
        return atoi(s);
    if (strncmp(s, "SIG", 3) == 0)
        s += 3;
    if (strcmp(s, "KILL") == 0) return SIGKILL;
    if (strcmp(s, "TERM") == 0) return SIGTERM;
    if (strcmp(s, "INT") == 0) return SIGINT;
    if (strcmp(s, "HUP") == 0) return SIGHUP;
    return -1;
}

int main(int argc, char **argv)
{
    int i = 1, sig = SIGTERM;
    if (i < argc && strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
        sig = parse_sig(argv[i + 1]);
        i += 2;
    } else if (i < argc && argv[i][0] == '-' && argv[i][1]) {
        sig = parse_sig(argv[i] + 1);
        i++;
    }
    if (sig <= 0 || i == argc) {
        fprintf(stderr, "usage: kill [-sig | -s sig] pid...\n");
        return 2;
    }
    int rc = 0;
    for (; i < argc; i++) {
        int pid = atoi(argv[i]);
        if (kill(pid, sig) < 0) {
            fprintf(stderr, "kill: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
