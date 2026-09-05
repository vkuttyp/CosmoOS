/* hello - the package system's smallest test subject. */
#include <stdio.h>

int main(int argc, char **argv)
{
    printf("hello, %s (hello 1.1)\n", argc > 1 ? argv[1] : "world");
    return 0;
}
