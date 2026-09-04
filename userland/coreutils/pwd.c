/* pwd - print the working directory. */
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    char buf[1024];
    if (getcwd(buf, sizeof(buf)) == NULL) {
        perror("pwd");
        return 1;
    }
    puts(buf);
    return 0;
}
