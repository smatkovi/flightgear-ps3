/* Minimal HTTP/1.1 downloader for the hangar (plain http, redirects followed).
   The socket is non-blocking after connect; http_step() reads what is there,
   so the hangar keeps drawing and the download can be cancelled. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <net/net.h>
#include <net/netdb.h>
#include <netinet/in.h>
#include "http.h"

enum { ST_IDLE, ST_HEADERS, ST_BODY, ST_DONE, ST_FAILED };

static int net_ready;

static int fail(http_dl *d, const char *msg)
{
    snprintf(d->err, sizeof d->err, "%s", msg);
    if (d->sock >= 0) { netClose(d->sock); d->sock = -1; }
    if (d->fd >= 0) { close(d->fd); d->fd = -1; }
    d->state = ST_FAILED;
    return -1;
}

/* "http://host[:port]/path" -> host, port, path */
static int parse_url(const char *url, char *host, int hostlen, int *port, char *path, int pathlen)
{
    const char *p, *slash, *colon;
    int n;
    if (strncasecmp(url, "http://", 7) != 0) return -1;
    p = url + 7;
    slash = strchr(p, '/');
    if (!slash) slash = p + strlen(p);
    colon = memchr(p, ':', slash - p);
    n = (int)((colon ? colon : slash) - p);
    if (n <= 0 || n >= hostlen) return -1;
    memcpy(host, p, n);
    host[n] = 0;
    *port = colon ? atoi(colon + 1) : 80;
    snprintf(path, pathlen, "%s", *slash ? slash : "/");
    return 0;
}

static int connect_and_send(http_dl *d, int port)
{
    struct hostent *he;
    struct sockaddr_in sa;
    char req[1400];
    int one = 1, len, sent = 0;

    he = gethostbyname(d->host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0])
        return fail(d, "server not found (is the PS3 online?)");
    d->sock = netSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (d->sock < 0) return fail(d, "no network socket");
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof sa.sin_addr);
    if (netConnect(d->sock, (struct sockaddr *)&sa, sizeof sa) < 0)
        return fail(d, "cannot connect to the server");
    len = snprintf(req, sizeof req,
                   "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: FlightGear-0.9.10-PS3\r\n"
                   "Accept: */*\r\nConnection: close\r\n\r\n", d->path, d->host);
    while (sent < len) {
        int n = netSend(d->sock, req + sent, len - sent, 0);
        if (n <= 0) return fail(d, "sending the request failed");
        sent += n;
    }
    netSetSockOpt(d->sock, SOL_SOCKET, SO_NBIO, &one, sizeof one);
    d->hdr_len = 0;
    d->total = -1;
    d->got = 0;
    d->state = ST_HEADERS;
    return 0;
}

int http_start(http_dl *d, const char *url, const char *outpath)
{
    int port;
    memset(d, 0, sizeof *d);
    d->sock = d->fd = -1;
    snprintf(d->out, sizeof d->out, "%s", outpath);
    if (!net_ready) {
        if (netInitialize() < 0) return fail(d, "network could not be started");
        net_ready = 1;
    }
    if (parse_url(url, d->host, sizeof d->host, &port, d->path, sizeof d->path) < 0)
        return fail(d, "unsupported address (only http:// works)");
    return connect_and_send(d, port);
}

/* Header block complete in d->hdr: handle status, redirects, Content-Length. */
static int got_headers(http_dl *d, char *body, int body_len)
{
    int status = 0, port;
    char *line, *next, loc[1024] = "";
    if (sscanf(d->hdr, "HTTP/%*d.%*d %d", &status) != 1) return fail(d, "not an HTTP answer");
    for (line = strstr(d->hdr, "\r\n"); line; line = next) {
        line += 2;
        next = strstr(line, "\r\n");
        if (!next) break;
        *next = 0;
        if (strncasecmp(line, "Content-Length:", 15) == 0) d->total = atoll(line + 15);
        else if (strncasecmp(line, "Location:", 9) == 0) {
            const char *v = line + 9;
            while (*v == ' ') v++;
            snprintf(loc, sizeof loc, "%s", v);
        } else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0 && strstr(line, "chunked"))
            return fail(d, "chunked answers are not supported");
        *next = '\r';
    }
    if (status >= 300 && status < 400 && loc[0]) {
        netClose(d->sock);
        d->sock = -1;
        if (++d->redirects > 5) return fail(d, "too many redirects");
        if (loc[0] == '/') {            /* same host */
            snprintf(d->path, sizeof d->path, "%s", loc);
            port = 80;
        } else if (parse_url(loc, d->host, sizeof d->host, &port, d->path, sizeof d->path) < 0)
            return fail(d, "redirected to https, which is not supported");
        return connect_and_send(d, port);
    }
    if (status != 200) {
        char m[64];
        snprintf(m, sizeof m, "server answered %d", status);
        return fail(d, m);
    }
    d->fd = open(d->out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (d->fd < 0) return fail(d, "cannot write the download to the hard disk");
    d->state = ST_BODY;
    if (body_len > 0) {
        if (write(d->fd, body, body_len) != body_len) return fail(d, "hard disk write failed");
        d->got += body_len;
    }
    return 1;
}

int http_step(http_dl *d)
{
    static char buf[65536];
    int rounds;
    if (d->state == ST_DONE) return 0;
    if (d->state == ST_FAILED || d->state == ST_IDLE) return -1;
    for (rounds = 0; rounds < 8; rounds++) {        /* up to 512 KB per frame */
        int n = netRecv(d->sock, buf, sizeof buf, 0);
        if (n < 0) {
            if (net_errno == NET_EWOULDBLOCK) return 1;
            return fail(d, "connection lost");
        }
        if (n == 0) {                               /* server closed: done? */
            if (d->state != ST_BODY) return fail(d, "connection closed early");
            if (d->total >= 0 && d->got != d->total) return fail(d, "download incomplete");
            close(d->fd);
            d->fd = -1;
            netClose(d->sock);
            d->sock = -1;
            d->state = ST_DONE;
            return 0;
        }
        if (d->state == ST_HEADERS) {
            char *end;
            int take = n;
            if (d->hdr_len + take >= (int)sizeof d->hdr) take = sizeof d->hdr - 1 - d->hdr_len;
            memcpy(d->hdr + d->hdr_len, buf, take);
            d->hdr_len += take;
            d->hdr[d->hdr_len] = 0;
            end = strstr(d->hdr, "\r\n\r\n");
            if (!end) {
                if (d->hdr_len >= (int)sizeof d->hdr - 1) return fail(d, "answer header too long");
                continue;
            }
            {
                int hl = (int)(end + 4 - d->hdr);
                int body_off = hl - (d->hdr_len - take);   /* where the body starts in buf */
                end[2] = 0;
                if (got_headers(d, buf + body_off, n - body_off) < 0) return -1;
                if (d->state == ST_HEADERS) return 1;       /* redirected */
            }
        } else {
            if (write(d->fd, buf, n) != n) return fail(d, "hard disk write failed");
            d->got += n;
        }
        if (d->total >= 0 && d->got >= d->total && d->state == ST_BODY) {
            close(d->fd);
            d->fd = -1;
            netClose(d->sock);
            d->sock = -1;
            d->state = ST_DONE;
            return 0;
        }
    }
    return 1;
}

void http_cancel(http_dl *d)
{
    if (d->sock >= 0) netClose(d->sock);
    if (d->fd >= 0) close(d->fd);
    d->sock = d->fd = -1;
    if (d->out[0]) unlink(d->out);
    d->state = ST_IDLE;
}
