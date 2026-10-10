#include "screen.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

typedef struct {
    uint32_t ch;
    rgb fg, bg;
    uint8_t attr;
} cell;

static struct {
    color_mode mode;
    int w, h;
    cell *back, *front;
    bool full;
    char *out;
    size_t len, cap;
} S;

color_mode scr_detect_color(void) {
    const char *force = getenv("NEONPX_COLOR");
    if (force && *force) {
        if (str_iequal(force, "truecolor") || str_iequal(force, "24bit")) return COLOR_TRUE;
        if (str_iequal(force, "256")) return COLOR_256;
        if (str_iequal(force, "16") || str_iequal(force, "basic")) return COLOR_16;
        if (str_iequal(force, "none") || str_iequal(force, "off")) return COLOR_NONE;
    }
    const char *no = getenv("NO_COLOR");
    if (no && *no) return COLOR_NONE;
    const char *ct = getenv("COLORTERM");
    if (ct && (str_iequal(ct, "truecolor") || str_iequal(ct, "24bit"))) return COLOR_TRUE;
    const char *tp = getenv("TERM_PROGRAM");
    static const char *const true_programs[] = {"iTerm.app", "WezTerm", "ghostty",      "vscode", "Hyper",
                                                "Tabby",     "rio",     "WarpTerminal", "zed"};
    if (tp)
        for (size_t i = 0; i < ARRAY_LEN(true_programs); i++)
            if (str_iequal(tp, true_programs[i])) return COLOR_TRUE;
    const char *term = getenv("TERM");
    if (term) {
        static const char *const true_terms[] = {"kitty", "alacritty", "ghostty", "wezterm", "foot", "direct"};
        for (size_t i = 0; i < ARRAY_LEN(true_terms); i++)
            if (str_icontains(term, true_terms[i])) return COLOR_TRUE;
        if (strcmp(term, "dumb") == 0) return COLOR_NONE;
    }
    return COLOR_256;
}

void scr_init(color_mode mode) {
    memset(&S, 0, sizeof S);
    S.mode = mode;
    S.full = true;
}

color_mode scr_color_mode(void) { return S.mode; }

void scr_free(void) {
    free(S.back);
    free(S.front);
    free(S.out);
    S.back = S.front = NULL;
    S.out = NULL;
}

void scr_resize(int w, int h) {
    w = imax(1, w);
    h = imax(1, h);
    free(S.back);
    free(S.front);
    S.w = w;
    S.h = h;
    S.back = xcalloc((size_t)w * (size_t)h, sizeof(cell));
    S.front = xcalloc((size_t)w * (size_t)h, sizeof(cell));
    S.full = true;
}

int scr_w(void) { return S.w; }
int scr_h(void) { return S.h; }
void scr_invalidate(void) { S.full = true; }

void scr_clear(rgb fg, rgb bg) {
    size_t n = (size_t)S.w * (size_t)S.h;
    for (size_t i = 0; i < n; i++)
        S.back[i] = (cell){' ', fg, bg, 0};
}

void scr_set(int x, int y, uint32_t ch, rgb fg, rgb bg, uint8_t attr) {
    if (x < 0 || y < 0 || x >= S.w || y >= S.h) return;
    cell *c = &S.back[(size_t)y * (size_t)S.w + (size_t)x];
    c->ch = ch;
    if (fg != RGB_KEEP) c->fg = fg;
    if (bg != RGB_KEEP) c->bg = bg;
    c->attr = attr;
}

void scr_fill(int x, int y, int w, int h, uint32_t ch, rgb fg, rgb bg) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            scr_set(i, j, ch, fg, bg, 0);
}

int scr_text(int x, int y, int maxw, const char *s, rgb fg, rgb bg, uint8_t attr) {
    int col = 0;
    while (*s && col < maxw) {
        uint32_t cp;
        s += utf8_decode(s, &cp);
        int w = cp_width(cp);
        if (w == 0) continue;
        /* Only single-width glyphs reach the grid; controls and wide chars become a dot. */
        if (w != 1) cp = 0xB7;
        scr_set(x + col, y, cp, fg, bg, attr);
        col++;
    }
    return col;
}

int text_width(const char *s) {
    int col = 0;
    while (*s) {
        uint32_t cp;
        s += utf8_decode(s, &cp);
        if (cp_width(cp) != 0) col++;
    }
    return col;
}

rgb rgb_mix(rgb a, rgb b, float t) {
    if (a == RGB_DEFAULT || b == RGB_DEFAULT) return t < 0.5f ? a : b;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    int r = ar + (int)((float)(br - ar) * t);
    int g = ag + (int)((float)(bg - ag) * t);
    int bl = ab + (int)((float)(bb - ab) * t);
    return (rgb)((r << 16) | (g << 8) | bl);
}

static int sq(int v) { return v * v; }

static int to256(rgb c) {
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    static const int lv[6] = {0, 95, 135, 175, 215, 255};
#define CUBE(v) ((v) < 48 ? 0 : (v) < 115 ? 1 : ((v) - 35) / 40)
    int ri = CUBE(r), gi = CUBE(g), bi = CUBE(b);
#undef CUBE
    int dc = sq(r - lv[ri]) + sq(g - lv[gi]) + sq(b - lv[bi]);
    int avg = (r + g + b) / 3;
    int gray = avg > 238 ? 23 : avg < 8 ? 0 : (avg - 8) / 10;
    int gv = 8 + gray * 10;
    int dg = sq(r - gv) + sq(g - gv) + sq(b - gv);
    return dg < dc ? 232 + gray : 16 + 36 * ri + 6 * gi + bi;
}

static int to16(rgb c) {
    static const rgb pal[16] = {0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
                                0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00, 0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff};
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255, best = 0, bd = 1 << 30;
    for (int i = 0; i < 16; i++) {
        int d = sq(r - (int)((pal[i] >> 16) & 255)) + sq(g - (int)((pal[i] >> 8) & 255)) + sq(b - (int)(pal[i] & 255));
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

#define SGR_CAP 96

static int sgr_color(char *o, size_t cap, rgb c, bool bg) {
    if (S.mode == COLOR_NONE) return 0;
    if (c == RGB_DEFAULT || c == RGB_KEEP) return snprintf(o, cap, bg ? ";49" : ";39");
    unsigned r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    switch (S.mode) {
    case COLOR_TRUE: return snprintf(o, cap, ";%d;2;%u;%u;%u", bg ? 48 : 38, r, g, b);
    case COLOR_256: return snprintf(o, cap, ";%d;5;%d", bg ? 48 : 38, to256(c));
    default: {
        int i = to16(c);
        return snprintf(o, cap, ";%d", (bg ? 40 : 30) + (i & 7) + (i >= 8 ? 60 : 0));
    }
    }
}

int scr_sgr(char *o, rgb fg, rgb bg, uint8_t attr) {
    static const struct {
        uint8_t bit;
        const char *code;
    } attrs[] = {{ATTR_BOLD, ";1"}, {ATTR_DIM, ";2"}, {ATTR_ITALIC, ";3"}, {ATTR_UNDER, ";4"}, {ATTR_REVERSE, ";7"}};
    size_t n = 0;
    memcpy(o, "\x1b[0", 3);
    n = 3;
    for (size_t i = 0; i < ARRAY_LEN(attrs); i++) {
        if (!(attr & attrs[i].bit)) continue;
        memcpy(o + n, attrs[i].code, 2);
        n += 2;
    }
    n += (size_t)sgr_color(o + n, SGR_CAP - n, fg, false);
    n += (size_t)sgr_color(o + n, SGR_CAP - n, bg, true);
    o[n++] = 'm';
    o[n] = '\0';
    return (int)n;
}

static void emit(const char *s, size_t n) {
    if (S.len + n > S.cap) {
        while (S.len + n > S.cap)
            S.cap = S.cap ? S.cap * 2 : 65536;
        S.out = xrealloc(S.out, S.cap);
    }
    memcpy(S.out + S.len, s, n);
    S.len += n;
}

void scr_flush(int fd) {
    S.len = 0;
    emit("\x1b[?2026h", 8); /* synchronized update: no tearing on terminals that support it */
    int cx = -1, cy = -1;
    bool have_style = false;
    cell style = {0};
    char buf[SGR_CAP];
    for (int y = 0; y < S.h; y++) {
        for (int x = 0; x < S.w; x++) {
            size_t i = (size_t)y * (size_t)S.w + (size_t)x;
            cell *c = &S.back[i];
            cell *f = &S.front[i];
            if (!S.full && c->ch == f->ch && c->fg == f->fg && c->bg == f->bg && c->attr == f->attr) continue;
            if (cx != x || cy != y) emit(buf, (size_t)snprintf(buf, sizeof buf, "\x1b[%d;%dH", y + 1, x + 1));
            if (!have_style || style.fg != c->fg || style.bg != c->bg || style.attr != c->attr) {
                emit(buf, (size_t)scr_sgr(buf, c->fg, c->bg, c->attr));
                style = *c;
                have_style = true;
            }
            char enc[4];
            int elen = utf8_encode(c->ch ? c->ch : ' ', enc);
            emit(enc, (size_t)elen);
            *f = *c;
            cx = x + 1;
            cy = y;
        }
    }
    emit("\x1b[0m\x1b[?2026l", 12);
    S.full = false;
    size_t off = 0;
    while (off < S.len) {
        ssize_t w = write(fd, S.out + off, S.len - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        off += (size_t)w;
    }
}
