/* Minimal HTTP/1.1 downloader for the hangar (plain http, redirects followed).
   http_step() does a little work per call, so the caller can keep drawing. */
#ifndef HANGAR_HTTP_H
#define HANGAR_HTTP_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int state;                  /* internal */
    int sock, fd, redirects;
    char host[128], path[1024], out[512];
    long long total;            /* Content-Length, -1 if unknown */
    long long got;              /* body bytes written so far */
    char err[200];
    char hdr[8192];
    int hdr_len;
} http_dl;

/* Start fetching url into the file outpath. Returns 0, or -1 with d->err set. */
int http_start(http_dl *d, const char *url, const char *outpath);
/* Continue: 1 while running, 0 when complete, -1 on error (d->err). */
int http_step(http_dl *d);
/* Stop and remove the partial file. */
void http_cancel(http_dl *d);

#ifdef __cplusplus
}
#endif
#endif
