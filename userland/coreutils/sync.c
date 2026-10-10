/* sync - commit every filesystem to its disk (roadmap M2: a disk root
 * keeps only what has been committed). */
#include <unistd.h>

int main(void)
{
    sync();
    return 0;
}
