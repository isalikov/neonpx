#ifndef NEONPX_HTTP_H
#define NEONPX_HTTP_H

#include <stddef.h>

/* Parses a complete HTTP/1.x response (Content-Length, chunked, or read-to-EOF).
 * On success *body is a malloc'ed, NUL-terminated buffer. Returns 0 or -1. */
int http_parse_response(const char *buf, size_t len, int *status, char **body, size_t *body_len);

/* One HTTP/1.0 request over a unix socket. Returns 0 when a response was parsed. */
int http_unix(const char *sock_path, const char *method, const char *path, int timeout_ms, int *status, char **body,
              size_t *body_len);

#endif
