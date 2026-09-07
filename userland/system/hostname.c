/* hostname - print the machine's name, or set it: hostname [name]
 *
 * The name comes from the caller's uts namespace, so a process started
 * with a namespace of its own prints its container's name rather than
 * the host's (docs/kernel/security/design.md §1e).
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr, "usage: hostname [name]\n");
        return 2;
    }
    if (argc == 2) {
        if (sethostname(argv[1], strlen(argv[1])) < 0) {
            perror("hostname");
            return 1;
        }
        return 0;
    }
    char name[HOST_NAME_MAX];
    if (gethostname(name, sizeof(name)) < 0) {
        perror("hostname");
        return 1;
    }
    puts(name);
    return 0;
}
