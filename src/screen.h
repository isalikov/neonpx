#ifndef NEONPX_SCREEN_H
#define NEONPX_SCREEN_H

#include <stdint.h>

/* Double-buffered cell grid; flush writes only what changed since the last frame. */

typedef uint32_t rgb;
#define RGB_DEFAULT 0xFF000000u /* terminal default color */
#define RGB_KEEP    0xFE000000u /* leave the cell's current color */

enum { ATTR_BOLD = 1, ATTR_DIM = 2, ATTR_UNDER = 4, ATTR_ITALIC = 8, ATTR_REVERSE = 16 };

typedef enum { COLOR_NONE, COLOR_16, COLOR_256, COLOR_TRUE } color_mode;

color_mode scr_detect_color(void);
void scr_init(color_mode mode);
color_mode scr_color_mode(void);
void scr_free(void);
void scr_resize(int w, int h);
int scr_w(void);
int scr_h(void);
void scr_clear(rgb fg, rgb bg);
void scr_set(int x, int y, uint32_t ch, rgb fg, rgb bg, uint8_t attr);
void scr_fill(int x, int y, int w, int h, uint32_t ch, rgb fg, rgb bg);
/* Writes UTF-8 text clipped to maxw columns; returns the columns used. */
int scr_text(int x, int y, int maxw, const char *s, rgb fg, rgb bg, uint8_t attr);
/* Column width of UTF-8 text as scr_text would draw it. */
int text_width(const char *s);
void scr_invalidate(void);
void scr_flush(int fd);

rgb rgb_mix(rgb a, rgb b, float t);
/* Builds the SGR sequence for a style into out (>= 96 bytes); exposed for tests. */
int scr_sgr(char *out, rgb fg, rgb bg, uint8_t attr);

#endif
