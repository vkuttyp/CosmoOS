/* dmesg - print the kernel log ring. */
#include <cosmo/klog.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void)
{
    size_t cap = 32768;
    char *buf = malloc(cap);
    if (buf == NULL) {
        fprintf(stderr, "dmesg: out of memory\n");
        return 1;
    }
    ssize_t n = klog_read(buf, cap);
    if (n < 0) {
        perror("dmesg");
        return 1;
    }
    ssize_t done = 0;
    while (done < n) {
        ssize_t w = write(1, buf + done, (size_t)(n - done));
        if (w <= 0)
            return 1;
        done += w;
    }
    free(buf);
    return 0;
}
