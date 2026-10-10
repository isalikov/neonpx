#include "term.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "util.h"

static struct termios orig;
static bool active;
static volatile sig_atomic_t resized, quit;
static unsigned char ibuf[256];
static size_t ilen;

static void on_winch(int sig) {
    (void)sig;
    resized = 1;
}

static void on_quit(int sig) {
    (void)sig;
    quit = 1;
}

static void write_str(const char *s) {
    size_t n = strlen(s);
    while (n) {
        ssize_t w = write(STDOUT_FILENO, s, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

int term_init(void) {
    if (tcgetattr(STDIN_FILENO, &orig) != 0) return -1;
    struct termios raw = orig;
    raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(tcflag_t)OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return -1;
    active = true;
    atexit(term_restore);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_winch; /* no SA_RESTART: poll() must wake up */
    sigaction(SIGWINCH, &sa, NULL);
    sa.sa_handler = on_quit;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* alt screen, hide cursor, no autowrap, SGR mouse */
    write_str("\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[?1000h\x1b[?1006h\x1b[2J");
    return 0;
}

void term_restore(void) {
    if (!active) return;
    active = false;
    write_str("\x1b[?1006l\x1b[?1000l\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l");
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);
}

void term_size(int *w, int *h) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
        *w = ws.ws_col;
        *h = ws.ws_row;
    } else {
        *w = 80;
        *h = 24;
    }
}

bool term_quit_requested(void) { return quit != 0; }

static int parse_int(const unsigned char **p, const unsigned char *end) {
    int v = 0;
    while (*p < end && **p >= '0' && **p <= '9') {
        if (v < 100000) v = v * 10 + (**p - '0');
        (*p)++;
    }
    return v;
}

size_t key_parse(const unsigned char *b, size_t n, key_event *ev, bool final) {
    memset(ev, 0, sizeof *ev);
    if (!n) return 0;
    unsigned char c = b[0];
    if (c == 0x1b) {
        if (n == 1) {
            if (!final) return 0;
            ev->type = KEY_ESC;
            return 1;
        }
        if (b[1] == '[') {
            size_t i = 2;
            while (i < n && !(b[i] >= 0x40 && b[i] <= 0x7e))
                i++;
            if (i >= n) {
                if (!final) return 0;
                ev->type = KEY_ESC;
                return 1;
            }
            unsigned char fin = b[i];
            const unsigned char *p = b + 2, *end = b + i;
            if (*p == '<') { /* SGR mouse: ESC [ < btn ; x ; y M|m */
                p++;
                int btn = parse_int(&p, end);
                if (p < end) p++;
                int x = parse_int(&p, end);
                if (p < end) p++;
                int y = parse_int(&p, end);
                if (fin == 'M') {
                    if (btn == 64)
                        ev->type = KEY_WHEEL_UP;
                    else if (btn == 65)
                        ev->type = KEY_WHEEL_DOWN;
                    else if ((btn & ~(4 | 8 | 16)) == 0) {
                        ev->type = KEY_CLICK;
                        ev->x = x - 1;
                        ev->y = y - 1;
                    }
                }
                return i + 1;
            }
            int p1 = parse_int(&p, end);
            switch (fin) {
            case 'A': ev->type = KEY_UP; break;
            case 'B': ev->type = KEY_DOWN; break;
            case 'C': ev->type = KEY_RIGHT; break;
            case 'D': ev->type = KEY_LEFT; break;
            case 'H': ev->type = KEY_HOME; break;
            case 'F': ev->type = KEY_END; break;
            case 'Z': ev->type = KEY_BTAB; break;
            case '~':
                switch (p1) {
                case 1:
                case 7: ev->type = KEY_HOME; break;
                case 4:
                case 8: ev->type = KEY_END; break;
                case 5: ev->type = KEY_PGUP; break;
                case 6: ev->type = KEY_PGDN; break;
                case 3: ev->type = KEY_DELETE; break;
                default: break;
                }
                break;
            default: break;
            }
            return i + 1;
        }
        if (b[1] == 'O') {
            if (n < 3) {
                if (!final) return 0;
                ev->type = KEY_ESC;
                return 1;
            }
            switch (b[2]) {
            case 'A': ev->type = KEY_UP; break;
            case 'B': ev->type = KEY_DOWN; break;
            case 'C': ev->type = KEY_RIGHT; break;
            case 'D': ev->type = KEY_LEFT; break;
            case 'H': ev->type = KEY_HOME; break;
            case 'F': ev->type = KEY_END; break;
            default: break;
            }
            return 3;
        }
        ev->type = KEY_ESC;
        return 1;
    }
    if (c == '\r' || c == '\n') {
        ev->type = KEY_ENTER;
        return 1;
    }
    if (c == '\t') {
        ev->type = KEY_TAB;
        return 1;
    }
    if (c == 0x7f || c == 0x08) {
        ev->type = KEY_BACKSPACE;
        return 1;
    }
    if (c < 0x20) {
        ev->type = KEY_CTRL;
        ev->ch = (uint32_t)(c + 'a' - 1);
        return 1;
    }
    if (c < 0x80) {
        ev->type = KEY_CHAR;
        ev->ch = c;
        return 1;
    }
    size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (n < need) {
        if (!final) return 0;
        return n;
    }
    uint32_t cp;
    int used = utf8_decode((const char *)b, &cp);
    ev->type = KEY_CHAR;
    ev->ch = cp;
    return (size_t)used;
}

bool term_wait(int timeout_ms, key_event *ev) {
    for (;;) {
        if (resized) {
            resized = 0;
            memset(ev, 0, sizeof *ev);
            ev->type = KEY_RESIZE;
            return true;
        }
        if (quit) return false;
        if (ilen) {
            size_t used = key_parse(ibuf, ilen, ev, ilen == sizeof ibuf);
            if (used) {
                memmove(ibuf, ibuf + used, ilen - used);
                ilen -= used;
                if (ev->type != KEY_NONE) return true;
                continue;
            }
        }
        struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
        /* A partial escape sequence gets a short grace period before it resolves to ESC. */
        int r = poll(&pfd, 1, ilen ? 30 : timeout_ms);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) {
            if (!ilen) return false;
            size_t used = key_parse(ibuf, ilen, ev, true);
            memmove(ibuf, ibuf + used, ilen - used);
            ilen -= used;
            if (ev->type != KEY_NONE) return true;
            continue;
        }
        ssize_t got = read(STDIN_FILENO, ibuf + ilen, sizeof ibuf - ilen);
        if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (got <= 0) return false;
        ilen += (size_t)got;
    }
}
