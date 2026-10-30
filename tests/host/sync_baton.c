/*
 * sync_baton.c - The pthreads under tests/host/sync_sched.c, apart from the
 * kernel headers (the host's <signal.h>, which <pthread.h> includes, and the
 * kernel's define the same names). One baton: only the thread holding it runs.
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>

#include "sync_baton.h"

static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_holder = -2;   /* -2: nobody yet; -1: the run is over */
static pthread_t g_threads[64];

void baton_give(int to)
{
    pthread_mutex_lock(&g_m);
    g_holder = to;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_m);
}

bool baton_wait(int self)
{
    pthread_mutex_lock(&g_m);
    while (g_holder != self && g_holder != -1)
        pthread_cond_wait(&g_cv, &g_m);
    bool mine = g_holder == self;
    pthread_mutex_unlock(&g_m);
    return mine;
}

void baton_exit(void) { pthread_exit(0); }

bool baton_env_on(const char *name)
{
    const char *v = getenv(name);
    return v != 0 && v[0] == '1';
}

void baton_reset(void)
{
    pthread_mutex_lock(&g_m);
    g_holder = -2;
    pthread_mutex_unlock(&g_m);
}

void baton_start(int n, void *(*entry)(void *), void *args[])
{
    for (int i = 0; i < n; i++)
        pthread_create(&g_threads[i], 0, entry, args[i]);
}

void baton_join(int n)
{
    for (int i = 0; i < n; i++)
        pthread_join(g_threads[i], 0);
}
