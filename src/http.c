#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "util.h"

#define HTTP_MAX_RESPONSE (16u << 20)

static const char *find_bytes(const char *hay, size_t n, const char *needle) {
    size_t l = strlen(needle);
    for (size_t i = 0; i + l <= n; i++)
        if (memcmp(hay + i, needle, l) == 0) return hay + i;
    return NULL;
}

static bool header_is(const char *line, size_t len, const char *name) {
    size_t nl = strlen(name);
    if (len <= nl || line[nl] != ':') return false;
    for (size_t i = 0; i < nl; i++)
        if (tolower((unsigned char)line[i]) != name[i]) return false;
    return true;
}

static int decode_chunked(const char *p, const char *end, char **body, size_t *body_len) {
    size_t cap = 4096, len = 0;
    char *out = xmalloc(cap);
    for (;;) {
        const char *eol = find_bytes(p, (size_t)(end - p), "\r\n");
        if (!eol) goto fail;
        size_t size = 0;
        const char *q = p;
        while (q < eol && isxdigit((unsigned char)*q)) {
            int c = tolower((unsigned char)*q);
            size = size * 16 + (size_t)(isdigit(c) ? c - '0' : c - 'a' + 10);
            if (size > HTTP_MAX_RESPONSE) goto fail;
            q++;
        }
        if (q == p) goto fail;
        p = eol + 2;
        if (size == 0) break;
        if ((size_t)(end - p) < size + 2) goto fail;
        if (len + size + 1 > cap) {
            while (len + size + 1 > cap)
                cap *= 2;
            out = xrealloc(out, cap);
        }
        memcpy(out + len, p, size);
        len += size;
        p += size + 2;
    }
    out[len] = '\0';
    *body = out;
    *body_len = len;
    return 0;
fail:
    free(out);
    return -1;
}

int http_parse_response(const char *buf, size_t len, int *status, char **body, size_t *body_len) {
    if (len < 12 || memcmp(buf, "HTTP/1.", 7) != 0) return -1;
    const char *hdr_end = find_bytes(buf, len, "\r\n\r\n");
    if (!hdr_end) return -1;
    if (!isdigit((unsigned char)buf[9]) || !isdigit((unsigned char)buf[10]) || !isdigit((unsigned char)buf[11]))
        return -1;
    *status = (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');

    bool chunked = false;
    long long content_length = -1;
    const char *line = find_bytes(buf, (size_t)(hdr_end - buf), "\r\n");
    while (line && line < hdr_end) {
        line += 2;
        const char *eol = find_bytes(line, (size_t)(hdr_end + 2 - line), "\r\n");
        if (!eol) break;
        size_t ll = (size_t)(eol - line);
        if (header_is(line, ll, "transfer-encoding")) {
            char val[64];
            str_ncopy(val, sizeof val, line + 18, ll - 18);
            if (str_icontains(val, "chunked")) chunked = true;
        } else if (header_is(line, ll, "content-length")) {
            char val[32];
            str_ncopy(val, sizeof val, line + 15, ll - 15);
            content_length = strtoll(val, NULL, 10);
        }
        line = eol;
    }

    const char *p = hdr_end + 4, *end = buf + len;
    if (chunked) return decode_chunked(p, end, body, body_len);
    size_t n = (size_t)(end - p);
    if (content_length >= 0) {
        if ((size_t)content_length > n) return -1;
        n = (size_t)content_length;
    }
    char *out = xmalloc(n + 1);
    memcpy(out, p, n);
    out[n] = '\0';
    *body = out;
    *body_len = n;
    return 0;
}

static bool write_all(int fd, const char *p, size_t n) {
    while (n) {
#ifdef MSG_NOSIGNAL
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
#else
        ssize_t w = send(fd, p, n, 0);
#endif
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

int http_unix(const char *sock_path, const char *method, const char *path, int timeout_ms, int *status, char **body,
              size_t *body_len) {
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof sa.sun_path) return -1;
    str_copy(sa.sun_path, sizeof sa.sun_path, sock_path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }

    char req[1024];
    int rl = snprintf(req, sizeof req,
                      "%s %s HTTP/1.0\r\nHost: docker\r\nUser-Agent: neonpx\r\nAccept: application/json\r\n"
                      "Content-Length: 0\r\n\r\n",
                      method, path);
    if (rl <= 0 || (size_t)rl >= sizeof req || !write_all(fd, req, (size_t)rl)) {
        close(fd);
        return -1;
    }

    size_t cap = 16384, len = 0;
    char *buf = xmalloc(cap);
    for (;;) {
        if (len == cap) {
            if (cap >= HTTP_MAX_RESPONSE) break;
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) {
            free(buf);
            close(fd);
            return -1;
        }
        if (r == 0) break;
        len += (size_t)r;
    }
    close(fd);
    int rc = http_parse_response(buf, len, status, body, body_len);
    free(buf);
    return rc;
}
