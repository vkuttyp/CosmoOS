/* lxhello - the smallest Linux program: write and exit_group. */
#include "lxabi.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    lx_puts("hello from linux abi\n");
    return 0;
}
