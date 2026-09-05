#ifndef _ARPA_INET_H
#define _ARPA_INET_H
#include <netinet/in.h>
#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46
int inet_pton(int family, const char *src, void *dst);
const char *inet_ntop(int family, const void *src, char *dst, socklen_t size);
#endif
