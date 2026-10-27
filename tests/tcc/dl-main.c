/* Roadmap 1.2 A1: a program built on the system that opens a library
 * built on the system (selfhost.sh's programs, libgreet.so). */
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: dl-main LIBRARY\n");
        return 2;
    }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (h == NULL) {
        fprintf(stderr, "dl-main: dlopen: %s\n", dlerror());
        return 1;
    }
    const char *(*greet)(void) = (const char *(*)(void))dlsym(h, "greet");
    int *answer = dlsym(h, "greet_answer");
    if (greet == NULL || answer == NULL) {
        fprintf(stderr, "dl-main: dlsym: %s\n", dlerror());
        return 1;
    }
    printf("cc-dlopen: %s, %d\n", greet(), *answer);
    return dlclose(h) == 0 ? 0 : 1;
}
