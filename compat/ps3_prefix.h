/* Force-included into every C++ file: 2006-era code relies on <iostream> and
   friends dragging in the C library declarations, which libstdc++ 7 no longer does. */
#ifndef PS3_PREFIX_H
#define PS3_PREFIX_H
#ifdef __cplusplus
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#endif
#ifndef HUGE
#define HUGE HUGE_VAL
#endif
#ifdef __cplusplus
extern "C" {
#endif
int gethostname(char *name, size_t len);
/* debug line to the TTY, port/ps3_debug.h */
void ps3_tty(const char *fmt, ...);
#ifdef __cplusplus
}
#endif
#endif
