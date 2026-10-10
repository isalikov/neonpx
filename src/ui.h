#ifndef NEONPX_UI_H
#define NEONPX_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "app.h"
#include "model.h"
#include "theme.h"

typedef struct {
    const theme *theme;
    bool transparent; /* keep the terminal's own background */
    const char *version;
} ui_opts;

/* Draws one frame into the screen buffer. Updates app list geometry (list_y, list_rows). */
void ui_render(app *a, const model *m, const ui_opts *o, uint64_t now);

#endif
