/* Unpack a zip archive (stored or deflated entries) for the hangar.
   Aircraft archives are a few MB, so the whole file is read into memory. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <zlib.h>
#include "unzip.h"

static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

int mkdir_p(const char *path)
{
    char tmp[1024];
    char *p;
    snprintf(tmp, sizeof tmp, "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return (mkdir(tmp, 0755) == 0 || access(tmp, F_OK) == 0) ? 0 : -1;
}

int remove_tree(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    char sub[1024];
    if (!d) return unlink(path);
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        if (remove_tree(sub) != 0) unlink(sub);
    }
    closedir(d);
    return rmdir(path);
}

static int seterr(char *err, int errlen, const char *msg)
{
    snprintf(err, errlen, "%s", msg);
    return -1;
}

int unzip_all(const char *zippath, const char *destdir, char *topdir, int topdir_len,
              char *err, int errlen, void (*progress)(int done, int total))
{
    unsigned char *zip = NULL, *eocd = NULL, *cd;
    long size, i;
    unsigned entries, cd_off, e;
    int fd, ok = -1;
    char top[256] = "";
    int single_top = 1;

    fd = open(zippath, O_RDONLY);
    if (fd < 0) return seterr(err, errlen, "cannot open the downloaded file");
    size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (size < 22 || size > 256L * 1024 * 1024) { close(fd); return seterr(err, errlen, "not a usable zip file"); }
    zip = (unsigned char *)malloc(size);
    if (!zip) { close(fd); return seterr(err, errlen, "not enough memory to unpack"); }
    if (read(fd, zip, size) != size) { close(fd); free(zip); return seterr(err, errlen, "reading the download failed"); }
    close(fd);

    for (i = size - 22; i >= 0 && i >= size - 22 - 65535; i--)
        if (rd32(zip + i) == 0x06054b50) { eocd = zip + i; break; }
    if (!eocd) { seterr(err, errlen, "broken zip file (no directory)"); goto out; }
    entries = rd16(eocd + 10);
    cd_off = rd32(eocd + 16);
    if (cd_off >= (unsigned long)size) { seterr(err, errlen, "broken zip file"); goto out; }

    cd = zip + cd_off;
    for (e = 0; e < entries; e++) {
        unsigned method, csize, usize, nlen, xlen, clen, lho, lnlen, lxlen;
        char name[512], out[1024];
        const unsigned char *data;
        if (cd + 46 > zip + size || rd32(cd) != 0x02014b50) { seterr(err, errlen, "broken zip directory"); goto out; }
        method = rd16(cd + 10);
        csize = rd32(cd + 20);
        usize = rd32(cd + 24);
        nlen = rd16(cd + 28);
        xlen = rd16(cd + 30);
        clen = rd16(cd + 32);
        lho = rd32(cd + 42);
        if (nlen == 0 || nlen >= sizeof name) { seterr(err, errlen, "bad file name in zip"); goto out; }
        memcpy(name, cd + 46, nlen);
        name[nlen] = 0;
        cd += 46 + nlen + xlen + clen;

        /* stay inside destdir */
        if (name[0] == '/' || strstr(name, "..") || strchr(name, '\\')) continue;
        {
            char *slash = strchr(name, '/');
            size_t tl = slash ? (size_t)(slash - name) : strlen(name);
            if (!slash) single_top = 0;
            else if (!top[0] && tl < sizeof top) { memcpy(top, name, tl); top[tl] = 0; }
            else if (strncmp(top, name, tl) != 0 || top[tl] != 0) single_top = 0;
        }
        snprintf(out, sizeof out, "%s/%s", destdir, name);
        if (name[nlen - 1] == '/') { mkdir_p(out); continue; }
        {
            char *ls = strrchr(out, '/');
            *ls = 0;
            mkdir_p(out);
            *ls = '/';
        }

        if ((unsigned long)lho + 30 > (unsigned long)size || rd32(zip + lho) != 0x04034b50) { seterr(err, errlen, "broken zip entry"); goto out; }
        lnlen = rd16(zip + lho + 26);
        lxlen = rd16(zip + lho + 28);
        data = zip + lho + 30 + lnlen + lxlen;
        if (data + csize > zip + size) { seterr(err, errlen, "zip file is truncated"); goto out; }

        fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { seterr(err, errlen, "cannot write to the hard disk"); goto out; }
        if (method == 0) {
            if (write(fd, data, csize) != (int)csize) { close(fd); seterr(err, errlen, "hard disk write failed"); goto out; }
        } else if (method == 8) {
            z_stream zs;
            unsigned char *buf = (unsigned char *)malloc(usize ? usize : 1);
            int r;
            if (!buf) { close(fd); seterr(err, errlen, "not enough memory to unpack"); goto out; }
            memset(&zs, 0, sizeof zs);
            inflateInit2(&zs, -MAX_WBITS);
            zs.next_in = (unsigned char *)data;
            zs.avail_in = csize;
            zs.next_out = buf;
            zs.avail_out = usize;
            r = inflate(&zs, Z_FINISH);
            inflateEnd(&zs);
            if (r != Z_STREAM_END || zs.total_out != usize ||
                write(fd, buf, usize) != (int)usize) {
                free(buf);
                close(fd);
                seterr(err, errlen, r != Z_STREAM_END ? "damaged data in zip" : "hard disk write failed");
                goto out;
            }
            free(buf);
        } else {
            close(fd);
            unlink(out);
            continue;           /* unsupported compression: skip the file */
        }
        close(fd);
        if (progress && (e % 16) == 0) progress(e, entries);
    }
    if (topdir) snprintf(topdir, topdir_len, "%s", single_top ? top : "");
    ok = 0;
out:
    free(zip);
    return ok;
}
