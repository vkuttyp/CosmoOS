/* hello 1.0 - the previous version of the package system's test subject. */
#include <stdio.h>

int main(int argc, char **argv)
{
    printf("hello, %s (hello 1.0)\n", argc > 1 ? argv[1] : "world");
    return 0;
}
