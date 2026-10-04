/* C library functions PSL1GHT's newlib declares but does not provide. */
#include <string.h>
#include <unistd.h>

int gethostname(char *name, size_t len)
{
    strncpy(name, "ps3", len);
    if (len) name[len - 1] = 0;
    return 0;
}

/* ---- debug helpers (ps3_debug.h) ---- */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/tty.h>
#include <sys/syscalls.h>
#include "ps3_debug.h"

void ps3_tty(const char *fmt, ...)
{
    char buf[512];
    u32 written;
    int n;
    va_list ap;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
    buf[n++] = '\n';
    sysTtyWrite(0, buf, n, &written);
}

#ifdef PS3_DEBUG
/* Corruption guards and an allocation tracer, built with make PS3_DEBUG=1
   (which also links with -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc). */

static struct __syscalls_t syscalls_copy;

/* .got (the TOC) never changes at run time in a static executable; a change
   means a stray write. The bounds come from the link (--defsym, see libs.mk). */
extern char ps3_got_start[], ps3_got_end[];
#define PS3_GOT_START ((unsigned long)ps3_got_start)
#define PS3_GOT_END ((unsigned long)ps3_got_end)
static unsigned long long *got_copy;

static void got_save(void)
{
    size_t n = PS3_GOT_END - PS3_GOT_START;
    if (!n) return;
    got_copy = (unsigned long long *)malloc(n);
    if (got_copy) memcpy(got_copy, (const void *)PS3_GOT_START, n);
    ps3_tty("got: watching %lx..%lx", (unsigned long)PS3_GOT_START, (unsigned long)PS3_GOT_END);
}

static void got_check(const char *where)
{
    const unsigned long long *now = (const unsigned long long *)PS3_GOT_START;
    size_t i, n = (PS3_GOT_END - PS3_GOT_START) / 8, changed = 0;
    if (!got_copy) return;
    for (i = 0; i < n; i++) {
        if (now[i] != got_copy[i]) {
            if (changed < 8)
                ps3_tty("got: CHANGED at %s: %lx: %016llx -> %016llx", where,
                        (unsigned long)(PS3_GOT_START + i * 8), got_copy[i], now[i]);
            got_copy[i] = now[i];
            changed++;
        }
    }
    if (changed) ps3_tty("got: %u words changed at %s", (unsigned)changed, where);
}

void ps3_syscalls_save(void)
{
    got_save();
    memcpy(&syscalls_copy, &__syscalls, sizeof syscalls_copy);
    ps3_tty("syscalls: table at %p, sys_lwmutex_lock_r = %p", (void *)&__syscalls,
            (void *)__syscalls.sys_lwmutex_lock_r);
}

/* Offset of newlib's __malloc_lock_object from __syscalls in the current link
   (from nm); only used for the debug dump below. */
#ifndef PS3_MALLOC_LOCK_OFS
#define PS3_MALLOC_LOCK_OFS 0x1e0
#endif

static void dump_state(const char *where)
{
    unsigned long long r13;
    const unsigned long long *ml = (const unsigned long long *)((const char *)&__syscalls + PS3_MALLOC_LOCK_OFS);
    static unsigned long long last[3];
    static void *last_reent;
    void *re = __getreent();
    __asm__ volatile("mr %0,13" : "=r"(r13));
    if (ml[0] != last[0] || ml[1] != last[1] || ml[2] != last[2] || re != last_reent) {
        ps3_tty("state at %s: r13=%llx reent=%p malloc_lock=%016llx %016llx %016llx",
                where, r13, re, ml[0], ml[1], ml[2]);
        last[0] = ml[0]; last[1] = ml[1]; last[2] = ml[2]; last_reent = re;
    }
}

void ps3_syscalls_check(const char *where)
{
    got_check(where);
    dump_state(where);
    const unsigned long long *now = (const unsigned long long *)&__syscalls;
    unsigned long long *old = (unsigned long long *)&syscalls_copy;
    unsigned i, n = sizeof syscalls_copy / 8;
    for (i = 0; i < n; i++) {
        if (now[i] != old[i]) {
            ps3_tty("syscalls: CHANGED at %s: offset %u: %016llx -> %016llx", where, i * 8, old[i], now[i]);
            old[i] = now[i];
        }
    }
}

/* ---- big-allocation tracer: link with -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc ---- */
void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t m);
void *__real_realloc(void *p, size_t n);
static int in_trace;

static void trace_big(const char *what, size_t n, void *p)
{
    if (n < (4u << 20) || in_trace) return;
    in_trace = 1;
    ps3_tty("%s(%lu KB) = %p from %p <- %p <- %p", what, (unsigned long)(n >> 10), p,
            __builtin_return_address(1), __builtin_return_address(2), __builtin_return_address(3));
    in_trace = 0;
}

void *__wrap_malloc(size_t n) { void *p = __real_malloc(n); trace_big("malloc", n, p); return p; }
void *__wrap_calloc(size_t n, size_t m) { void *p = __real_calloc(n, m); trace_big("calloc", n * m, p); return p; }
void *__wrap_realloc(void *q, size_t n) { void *p = __real_realloc(q, n); trace_big("realloc", n, p); return p; }

#endif /* PS3_DEBUG */
