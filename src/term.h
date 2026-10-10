#ifndef NEONPX_TERM_H
#define NEONPX_TERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    KEY_NONE,
    KEY_CHAR,
    KEY_CTRL, /* ch = 'a'..'z' */
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PGUP,
    KEY_PGDN,
    KEY_ENTER,
    KEY_ESC,
    KEY_TAB,
    KEY_BTAB,
    KEY_BACKSPACE,
    KEY_DELETE,
    KEY_WHEEL_UP,
    KEY_WHEEL_DOWN,
    KEY_CLICK, /* x, y: zero-based cell */
    KEY_RESIZE,
} key_type;

typedef struct {
    key_type type;
    uint32_t ch;
    int x, y;
} key_event;

int term_init(void);
void term_restore(void);
void term_size(int *w, int *h);
/* Waits up to timeout_ms for input. False on timeout or when a quit signal arrived. */
bool term_wait(int timeout_ms, key_event *ev);
bool term_quit_requested(void);

/* Decodes one event from buf. Returns bytes consumed; 0 means "need more bytes"
 * (unless `final`, which forces a lone ESC or a truncated sequence to resolve). */
size_t key_parse(const unsigned char *buf, size_t len, key_event *ev, bool final);

#endif
