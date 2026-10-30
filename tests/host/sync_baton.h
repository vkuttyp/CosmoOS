/* sync_baton.h - The baton of tests/host/sync_sched.c (sync_baton.c). */
#ifndef TESTS_HOST_SYNC_BATON_H
#define TESTS_HOST_SYNC_BATON_H

#include <stdbool.h>

void baton_reset(void);
void baton_give(int to);    /* -1 ends the run: every waiter returns false */
bool baton_wait(int self);  /* true once `self` holds the baton, false when the run ended */
void baton_exit(void);
bool baton_env_on(const char *name);   /* the variable is "1..." (getenv, which the kernel headers cannot sit beside) */
void baton_start(int n, void *(*entry)(void *), void *args[]);
void baton_join(int n);

#endif
