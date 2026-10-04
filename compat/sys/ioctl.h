/* PS3: no ioctl. Sockets are switched to non-blocking with setsockopt/fcntl instead. */
#ifndef PS3_SYS_IOCTL_H
#define PS3_SYS_IOCTL_H
#define FIONBIO 0x5421
static inline int ioctl(int fd, unsigned long req, ...) { (void)fd; (void)req; return -1; }
#endif
