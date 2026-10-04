/* Unpack a zip archive (stored or deflated entries) for the hangar. */
#ifndef HANGAR_UNZIP_H
#define HANGAR_UNZIP_H
#ifdef __cplusplus
extern "C" {
#endif

/* Extract zippath into destdir. topdir receives the archive's single top-level
   directory (empty if there is none). progress(done, total) is called now and
   then. Returns 0, or -1 with err set. */
int unzip_all(const char *zippath, const char *destdir, char *topdir, int topdir_len,
              char *err, int errlen, void (*progress)(int done, int total));

/* mkdir -p */
int mkdir_p(const char *path);
/* rm -r */
int remove_tree(const char *path);

#ifdef __cplusplus
}
#endif
#endif
