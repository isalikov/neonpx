#include "util.h"

#include <ctype.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t now_ms(void) { return now_ns() / 1000000ull; }

void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0) {
    }
}

static void oom(void) {
    fputs("NeonPX: out of memory\n", stderr);
    abort();
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) oom();
    return p;
}

void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p) oom();
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) oom();
    return q;
}

char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *d = xmalloc(n + 1);
    memcpy(d, s, n + 1);
    return d;
}

size_t str_ncopy(char *dst, size_t cap, const char *src, size_t n) {
    if (!cap) return 0;
    if (!src) n = 0;
    if (n >= cap) {
        n = cap - 1;
        /* Back off so a multi-byte sequence is never cut in half. */
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
            n--;
    }
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
}

size_t str_copy(char *dst, size_t cap, const char *src) { return str_ncopy(dst, cap, src, src ? strlen(src) : 0); }

void str_append(char *dst, size_t cap, size_t *len, const char *src, size_t n) {
    if (!cap || *len + 1 >= cap) return;
    size_t room = cap - 1 - *len;
    if (n > room) {
        n = room;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
            n--;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[(*len)++] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
    }
    dst[*len] = '\0';
}

bool str_icontains(const char *hay, const char *needle) {
    if (!needle || !*needle) return true;
    if (!hay) return false;
    size_t nl = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl) return true;
    }
    return false;
}

bool str_iequal(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

bool str_istarts(const char *s, const char *prefix) {
    for (; *prefix; s++, prefix++)
        if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
    return true;
}

bool str_ends(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && memcmp(s + a - b, suffix, b) == 0;
}

const char *path_base(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return path;
    if (slash[1]) return slash + 1;
    /* Trailing slash: walk back to the previous component. */
    const char *end = slash;
    while (end > path && end[-1] == '/')
        end--;
    const char *start = end;
    while (start > path && start[-1] != '/')
        start--;
    return start == end ? path : start;
}

void path_tilde(char *dst, size_t cap, const char *path) {
    const char *home = getenv("HOME");
    size_t hl = home ? strlen(home) : 0;
    if (hl > 1 && strncmp(path, home, hl) == 0 && (path[hl] == '/' || path[hl] == '\0')) {
        size_t len = 0;
        if (cap) dst[0] = '\0';
        str_append(dst, cap, &len, "~", 1);
        str_append(dst, cap, &len, path + hl, strlen(path + hl));
        return;
    }
    str_copy(dst, cap, path);
}

int utf8_decode(const char *str, uint32_t *cp) {
    const unsigned char *s = (const unsigned char *)str;
    unsigned char c = s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    int n;
    uint32_t v;
    if ((c & 0xE0) == 0xC0) {
        n = 2;
        v = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3;
        v = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        n = 4;
        v = c & 0x07;
    } else {
        *cp = 0xFFFD;
        return 1;
    }
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return i;
        }
        v = (v << 6) | (s[i] & 0x3F);
    }
    static const uint32_t min[] = {0, 0, 0x80, 0x800, 0x10000};
    if (v < min[n] || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) v = 0xFFFD;
    *cp = v;
    return n;
}

int utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp > 0x10FFFF) return utf8_encode(0xFFFD, out);
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Emoji-presentation characters below U+1F000 that terminals draw two cells wide. */
static const uint32_t wide_emoji[][2] = {
    {0x231A, 0x231B}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615},
    {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE},
    {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5},
    {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C},
    {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF},
    {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55},
};

int cp_width(uint32_t cp) {
    if (cp == 0) return 0;
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return -1;
    if ((cp >= 0x300 && cp <= 0x36F) || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0xFE00 && cp <= 0xFE0F)) return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F000 && cp <= 0x1FAFF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD))
        return 2;
    if (cp >= 0x231A && cp <= 0x2B55)
        for (size_t i = 0; i < ARRAY_LEN(wide_emoji); i++)
            if (cp >= wide_emoji[i][0] && cp <= wide_emoji[i][1]) return 2;
    return 1;
}

void fmt_bytes(char *dst, size_t cap, uint64_t b) {
    const double k = 1024.0;
    double v = (double)b;
    if (v < k)
        snprintf(dst, cap, "%lluB", (unsigned long long)b);
    else if (v < k * k)
        snprintf(dst, cap, "%.0fK", v / k);
    else if (v < k * k * k)
        snprintf(dst, cap, v / (k * k) < 10 ? "%.1fM" : "%.0fM", v / (k * k));
    else
        snprintf(dst, cap, "%.1fG", v / (k * k * k));
}

void fmt_duration(char *dst, size_t cap, int64_t s) {
    if (s < 0) s = 0;
    long long v = (long long)s;
    if (v < 60)
        snprintf(dst, cap, "%llds", v);
    else if (v < 3600)
        snprintf(dst, cap, "%lldm%02llds", v / 60, v % 60);
    else if (v < 86400)
        snprintf(dst, cap, "%lldh%02lldm", v / 3600, (v % 3600) / 60);
    else
        snprintf(dst, cap, "%lldd%02lldh", v / 86400, (v % 86400) / 3600);
}

const char *user_name(unsigned uid) {
    static struct {
        unsigned uid;
        char name[32];
    } cache[32];
    static int used;
    for (int i = 0; i < used; i++)
        if (cache[i].uid == uid) return cache[i].name;
    int slot = used < (int)ARRAY_LEN(cache) ? used++ : (int)(uid % ARRAY_LEN(cache));
    cache[slot].uid = uid;
    struct passwd *pw = getpwuid((uid_t)uid);
    if (pw && pw->pw_name)
        str_copy(cache[slot].name, sizeof cache[slot].name, pw->pw_name);
    else
        snprintf(cache[slot].name, sizeof cache[slot].name, "%u", uid);
    return cache[slot].name;
}
