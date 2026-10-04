/* PS3: socket bits newlib/PSL1GHT do not put where BSD code expects them. */
#ifndef PS3_NET_H
#define PS3_NET_H
#include <fcntl.h>
#include <net/net.h>
#include <net/select.h>
#ifndef O_NDELAY
#define O_NDELAY O_NONBLOCK
#endif
#ifdef __cplusplus
extern "C" {
#endif
int gethostname(char *name, size_t len);
#ifdef __cplusplus
}
#endif
#endif
