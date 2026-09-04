/* echo - write arguments to standard output. -n omits the newline. */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    int i = 1, nl = 1;
    if (i < argc && strcmp(argv[i], "-n") == 0) {
        nl = 0;
        i++;
    }
    for (; i < argc; i++) {
        fputs(argv[i], stdout);
        if (i + 1 < argc)
            fputc(' ', stdout);
    }
    if (nl)
        fputc('\n', stdout);
    return fflush(stdout) == 0 ? 0 : 1;
}
