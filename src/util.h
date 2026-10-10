#ifndef NEONPX_UTIL_H
#define NEONPX_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Severity of a status-line message. */
enum { LVL_INFO, LVL_OK, LVL_WARN, LVL_ERR };

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int iclamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

uint64_t now_ns(void);
uint64_t now_ms(void);
void sleep_ms(int ms);

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

/* Bounded copies that always NUL-terminate and never split a UTF-8 sequence. */
size_t str_copy(char *dst, size_t cap, const char *src);
size_t str_ncopy(char *dst, size_t cap, const char *src, size_t n);
/* Appends `src` (n bytes) to dst of length *len, replacing control bytes with spaces. */
void str_append(char *dst, size_t cap, size_t *len, const char *src, size_t n);

bool str_icontains(const char *hay, const char *needle);
bool str_iequal(const char *a, const char *b);
bool str_istarts(const char *s, const char *prefix);
bool str_ends(const char *s, const char *suffix);

const char *path_base(const char *path);
/* Replaces a leading $HOME with "~". */
void path_tilde(char *dst, size_t cap, const char *path);

/* Decodes one UTF-8 sequence; invalid input yields U+FFFD. Returns bytes used (>= 1). */
int utf8_decode(const char *s, uint32_t *cp);
int utf8_encode(uint32_t cp, char out[4]);
/* Terminal column width: -1 control, 0 combining, 1 normal, 2 wide. */
int cp_width(uint32_t cp);

void fmt_bytes(char *dst, size_t cap, uint64_t bytes);
void fmt_duration(char *dst, size_t cap, int64_t seconds);

const char *user_name(unsigned uid);

#endif
