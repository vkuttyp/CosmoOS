/* hello_musl - a real statically linked Linux program (built with musl-gcc -static when available). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

int main(void)
{
    struct utsname u;
    if (uname(&u) != 0) {
        perror("uname");
        return 1;
    }
    char *p = malloc(100);
    strcpy(p, "hello from musl");
    printf("%s on %s %s (pid %d)\n", p, u.sysname, u.machine, (int)getpid());
    free(p);
    return 0;
}
