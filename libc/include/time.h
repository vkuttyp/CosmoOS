#ifndef _TIME_H
#define _TIME_H
#include <sys/types.h>
struct timespec {
    time_t tv_sec;
    long tv_nsec;
};
typedef int clockid_t;
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
int nanosleep(const struct timespec *req, struct timespec *rem);
int clock_gettime(clockid_t clk, struct timespec *ts);
#endif
