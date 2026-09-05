/* fortune - print one line of /usr/share/fortunes/fortunes.txt, chosen by the clock. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/usr/share/fortunes/fortunes.txt";
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        perror(path);
        return 1;
    }
    char *lines[256];
    int n = 0;
    char buf[512];
    while (n < 256 && fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        if (len && buf[len - 1] == '\n')
            buf[len - 1] = '\0';
        if (buf[0])
            lines[n++] = strdup(buf);
    }
    fclose(f);
    if (n == 0) {
        fprintf(stderr, "fortune: %s is empty\n", path);
        return 1;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    puts(lines[(unsigned)(ts.tv_nsec / 1000) % (unsigned)n]);
    return 0;
}
