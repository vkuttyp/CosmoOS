/* sleep - pause for a number of seconds (fractions allowed: 0.5). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: sleep seconds\n");
        return 2;
    }
    char *end;
    long sec = strtol(argv[1], &end, 10);
    long nsec = 0;
    if (*end == '.') {
        long scale = 100000000L;
        for (end++; *end >= '0' && *end <= '9' && scale > 0; end++, scale /= 10)
            nsec += (*end - '0') * scale;
    }
    if (*end || sec < 0) {
        fprintf(stderr, "sleep: bad interval '%s'\n", argv[1]);
        return 2;
    }
    struct timespec ts = { .tv_sec = sec, .tv_nsec = nsec };
    return nanosleep(&ts, NULL) == 0 ? 0 : 1;
}
