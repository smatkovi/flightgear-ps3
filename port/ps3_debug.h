/* Debug helpers for the PS3 port that work even when the C library is broken. */
#ifndef PS3_DEBUG_H
#define PS3_DEBUG_H
#ifdef __cplusplus
extern "C" {
#endif

/* Formatted line to the TTY (RPCS3: TTY.log). Uses no heap and no stdio streams. */
void ps3_tty(const char *fmt, ...);
/* Remember the C library's system call table, then report any change to it. */
void ps3_syscalls_save(void);
void ps3_syscalls_check(const char *where);

#ifdef __cplusplus
}
#endif
#endif
