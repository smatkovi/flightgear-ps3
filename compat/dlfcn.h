/* PS3: no dynamic loading. */
#ifndef PS3_DLFCN_H
#define PS3_DLFCN_H
#define RTLD_NOW 0
#define RTLD_GLOBAL 0
#define RTLD_LAZY 0
#define RTLD_DEFAULT ((void *)0)
static inline void *dlopen(const char *n, int f) { (void)n; (void)f; return 0; }
static inline void *dlsym(void *h, const char *n) { (void)h; (void)n; return 0; }
static inline int dlclose(void *h) { (void)h; return 0; }
static inline char *dlerror(void) { return 0; }
#endif
