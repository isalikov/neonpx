#ifndef NEONPX_THEME_H
#define NEONPX_THEME_H

#include "classify.h"
#include "screen.h"

typedef struct {
    const char *name;
    rgb bg, bg_alt;     /* base and scanline rows */
    rgb panel;          /* header/detail surfaces */
    rgb head_a, head_b; /* header gradient */
    rgb border, text, dim, faint;
    rgb primary;   /* cyan */
    rgb secondary; /* magenta */
    rgb warn, ok, danger;
    rgb sel_bg;
    rgb rt[RT_COUNT];
} theme;

/* NULL when the name is unknown. */
const theme *theme_find(const char *name);
/* NULL or unknown name → the default "neon" theme. */
const theme *theme_get(const char *name);
const char *theme_names(void);

#endif
