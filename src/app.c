#include "app.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "kill.h"
#include "util.h"

#define STATUS_MS 4500

void app_init(app *a, view_mode mode) {
    memset(a, 0, sizeof *a);
    a->mode = mode;
    a->sort = SORT_CPU;
    a->list_rows = 10;
}

void app_free(app *a) {
    free(a->view);
    a->view = NULL;
}

const char *sort_name(sort_key k) {
    static const char *const names[SORT_COUNT] = {"CPU", "MEM", "PORT", "NAME", "PID", "UPTIME"};
    return k < SORT_COUNT ? names[k] : "?";
}

const char *mode_name(view_mode m) {
    static const char *const names[MODE_COUNT] = {"DEV", "PORTS", "ALL"};
    return m < MODE_COUNT ? names[m] : "?";
}

bool entry_in_mode(const entry *e, view_mode mode) {
    switch (mode) {
    case MODE_DEV:
        if (e->kind == ENT_CONTAINER) return e->running;
        if (e->kind == ENT_PORT) return true;
        return (e->rt != RT_NONE && e->rt != RT_PROXY) || e->nports > 0;
    case MODE_PORTS:
        if (e->kind == ENT_CONTAINER && !e->running) return false;
        return e->nports > 0;
    default: return true;
    }
}

bool entry_matches(const entry *e, const char *filter) {
    if (!filter || !*filter) return true;
    /* ":5173" matches a port prefix exactly. */
    if (filter[0] == ':' && filter[1]) {
        for (int i = 0; i < e->nports; i++) {
            char buf[8];
            snprintf(buf, sizeof buf, "%u", e->ports[i].port);
            if (str_istarts(buf, filter + 1)) return true;
        }
        return false;
    }
    if (str_icontains(e->title, filter) || str_icontains(e->name, filter) || str_icontains(e->project, filter) ||
        str_icontains(e->cmd, filter) || str_icontains(e->image, filter) || str_icontains(runtime_badge(e->rt), filter))
        return true;
    char buf[16];
    if (e->pid) {
        snprintf(buf, sizeof buf, "%d", e->pid);
        if (strcmp(buf, filter) == 0) return true;
    }
    for (int i = 0; i < e->nports; i++) {
        snprintf(buf, sizeof buf, "%u", e->ports[i].port);
        if (strcmp(buf, filter) == 0) return true;
    }
    return e->id[0] && str_istarts(e->id, filter);
}

static const model *sort_model;
static sort_key sort_by;
static bool sort_reverse;

static int cmp_num(double a, double b) { return (a > b) - (a < b); }

static int view_cmp(const void *pa, const void *pb) {
    const entry *a = &sort_model->items[*(const size_t *)pa];
    const entry *b = &sort_model->items[*(const size_t *)pb];
    int c = 0;
    switch (sort_by) {
    case SORT_CPU:
        c = -cmp_num(a->has_cpu ? a->cpu : -1, b->has_cpu ? b->cpu : -1);
        if (!c) c = -cmp_num((double)a->mem, (double)b->mem);
        break;
    case SORT_MEM: c = -cmp_num((double)a->mem, (double)b->mem); break;
    case SORT_PORT: {
        int pa_ = a->nports ? a->ports[0].port : 70000, pb_ = b->nports ? b->ports[0].port : 70000;
        c = (pa_ > pb_) - (pa_ < pb_);
        break;
    }
    case SORT_NAME: c = strcasecmp(a->title, b->title); break;
    case SORT_PID:
        c = (a->kind != b->kind) ? (int)a->kind - (int)b->kind : (a->pid > b->pid) - (a->pid < b->pid);
        break;
    case SORT_UPTIME: c = -cmp_num((double)a->start, (double)b->start); break;
    default: break;
    }
    if (sort_reverse) c = -c;
    if (!c) c = strcasecmp(a->title, b->title);
    if (!c) c = strcmp(a->key, b->key);
    return c;
}

static void clamp_scroll(app *a) {
    int n = (int)a->nview;
    if (n == 0) {
        a->sel = a->top = 0;
        return;
    }
    a->sel = iclamp(a->sel, 0, n - 1);
    int rows = imax(1, a->list_rows);
    if (a->sel < a->top) a->top = a->sel;
    if (a->sel >= a->top + rows) a->top = a->sel - rows + 1;
    a->top = iclamp(a->top, 0, imax(0, n - rows));
}

void app_rebuild(app *a, const model *m) {
    if (a->capview < m->n) {
        a->capview = m->n + 64;
        a->view = xrealloc(a->view, a->capview * sizeof *a->view);
    }
    a->nview = 0;
    memset(a->counts, 0, sizeof a->counts);
    for (size_t i = 0; i < m->n; i++) {
        const entry *e = &m->items[i];
        if (!entry_matches(e, a->filter)) continue;
        for (int md = 0; md < MODE_COUNT; md++)
            if (entry_in_mode(e, (view_mode)md)) a->counts[md]++;
        if (entry_in_mode(e, a->mode)) a->view[a->nview++] = i;
    }
    sort_model = m;
    sort_by = a->sort;
    sort_reverse = a->reverse;
    qsort(a->view, a->nview, sizeof *a->view, view_cmp);

    /* Follow the selected entry, wherever the new sort put it. */
    if (a->sel_key[0]) {
        for (size_t i = 0; i < a->nview; i++) {
            if (strcmp(m->items[a->view[i]].key, a->sel_key) == 0) {
                a->sel = (int)i;
                break;
            }
        }
    }
    clamp_scroll(a);
    if (a->nview)
        str_copy(a->sel_key, sizeof a->sel_key, m->items[a->view[a->sel]].key);
    else
        a->sel_key[0] = '\0';
}

const entry *app_selected(const app *a, const model *m) {
    if (!a->nview || a->sel < 0 || (size_t)a->sel >= a->nview) return NULL;
    return &m->items[a->view[a->sel]];
}

void app_status(app *a, int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->status, sizeof a->status, fmt, ap);
    va_end(ap);
    a->status_level = level;
    a->status_until = now_ms() + STATUS_MS;
}

static void select_index(app *a, const model *m, int idx) {
    if (!a->nview) return;
    a->sel = iclamp(idx, 0, (int)a->nview - 1);
    clamp_scroll(a);
    str_copy(a->sel_key, sizeof a->sel_key, m->items[a->view[a->sel]].key);
}

static void ask(app *a, const model *m, const entry *e, action_type t) {
    action *p = &a->pending;
    memset(p, 0, sizeof *p);
    p->type = t;
    p->kind = e->kind;
    p->pid = e->pid;
    p->start = e->start;
    p->proxy = e->rt == RT_PROXY;
    str_copy(p->id, sizeof p->id, e->id);
    str_copy(p->label, sizeof p->label, e->title);
    if (e->kind == ENT_PROC) {
        pid_ref refs[KILL_MAX];
        p->children = imax(0, kill_collect(m->procs, m->nprocs, e->pid, refs, KILL_MAX) - 1);
    }
    a->ovl = OVL_CONFIRM;
}

static action signal_request(app *a, const model *m, bool force) {
    action none = {0};
    const entry *e = app_selected(a, m);
    if (!e) return none;
    switch (e->kind) {
    case ENT_PROC: ask(a, m, e, force ? ACT_SIGKILL : ACT_SIGTERM); break;
    case ENT_CONTAINER:
        if (!e->running)
            app_status(a, LVL_INFO, "%s is not running (r starts it)", e->title);
        else
            ask(a, m, e, force ? ACT_DOCKER_KILL : ACT_DOCKER_STOP);
        break;
    case ENT_PORT:
        app_status(a, LVL_WARN, "owner of :%u is hidden from this user, try sudo neonpx", e->ports[0].port);
        break;
    }
    return none;
}

static void set_mode(app *a, const model *m, view_mode md) {
    a->mode = md;
    app_rebuild(a, m);
}

static action filter_key(app *a, const model *m, const key_event *ev) {
    action none = {0};
    size_t len = strlen(a->filter);
    switch (ev->type) {
    case KEY_ESC:
        a->filter[0] = '\0';
        a->editing = false;
        break;
    case KEY_ENTER:
    case KEY_TAB: a->editing = false; break;
    case KEY_BACKSPACE:
        while (len > 0 && ((unsigned char)a->filter[len - 1] & 0xC0) == 0x80)
            len--;
        if (len > 0) len--;
        a->filter[len] = '\0';
        break;
    case KEY_CTRL:
        if (ev->ch == 'u') a->filter[0] = '\0';
        if (ev->ch == 'c') {
            action q = {.type = ACT_QUIT};
            return q;
        }
        break;
    case KEY_CHAR: {
        char enc[4];
        int l = utf8_encode(ev->ch, enc);
        if (len + (size_t)l < sizeof a->filter) {
            memcpy(a->filter + len, enc, (size_t)l);
            a->filter[len + (size_t)l] = '\0';
        }
        break;
    }
    case KEY_UP:
    case KEY_DOWN: a->editing = false; return app_key(a, m, ev);
    default: break;
    }
    a->sel = 0;
    a->sel_key[0] = '\0';
    app_rebuild(a, m);
    return none;
}

action app_key(app *a, const model *m, const key_event *ev) {
    action none = {0}, act = {0};

    if (a->ovl == OVL_CONFIRM) {
        bool yes = (ev->type == KEY_CHAR && (ev->ch == 'y' || ev->ch == 'Y')) || ev->type == KEY_ENTER;
        a->ovl = OVL_NONE;
        if (yes) return a->pending;
        app_status(a, LVL_INFO, "aborted");
        return none;
    }
    if (a->ovl == OVL_HELP) {
        if (ev->type != KEY_RESIZE && ev->type != KEY_NONE) a->ovl = OVL_NONE;
        if (ev->type == KEY_CTRL && ev->ch == 'c') act.type = ACT_QUIT;
        return act;
    }
    if (a->editing) return filter_key(a, m, ev);

    int page = imax(1, a->list_rows - 1);
    switch (ev->type) {
    case KEY_UP: select_index(a, m, a->sel - 1); break;
    case KEY_DOWN: select_index(a, m, a->sel + 1); break;
    case KEY_PGUP: select_index(a, m, a->sel - page); break;
    case KEY_PGDN: select_index(a, m, a->sel + page); break;
    case KEY_HOME: select_index(a, m, 0); break;
    case KEY_END: select_index(a, m, (int)a->nview - 1); break;
    case KEY_WHEEL_UP: select_index(a, m, a->sel - 3); break;
    case KEY_WHEEL_DOWN: select_index(a, m, a->sel + 3); break;
    case KEY_CLICK: {
        int row = ev->y - a->list_y;
        if (row >= 0 && row < a->list_rows) select_index(a, m, a->top + row);
        break;
    }
    case KEY_TAB: set_mode(a, m, (view_mode)((a->mode + 1) % MODE_COUNT)); break;
    case KEY_BTAB: set_mode(a, m, (view_mode)((a->mode + MODE_COUNT - 1) % MODE_COUNT)); break;
    case KEY_ESC:
        if (a->filter[0]) {
            a->filter[0] = '\0';
            app_rebuild(a, m);
        }
        break;
    case KEY_CTRL:
        switch (ev->ch) {
        case 'c': act.type = ACT_QUIT; return act;
        case 'd': select_index(a, m, a->sel + page / 2); break;
        case 'u': select_index(a, m, a->sel - page / 2); break;
        case 'l': act.type = ACT_REFRESH; return act;
        default: break;
        }
        break;
    case KEY_CHAR:
        switch (ev->ch) {
        case 'q': act.type = ACT_QUIT; return act;
        case 'j': select_index(a, m, a->sel + 1); break;
        case 'k': select_index(a, m, a->sel - 1); break;
        case 'g': select_index(a, m, 0); break;
        case 'G': select_index(a, m, (int)a->nview - 1); break;
        case '1': set_mode(a, m, MODE_DEV); break;
        case '2': set_mode(a, m, MODE_PORTS); break;
        case '3': set_mode(a, m, MODE_ALL); break;
        case '/': a->editing = true; break;
        case 's':
            a->sort = (sort_key)((a->sort + 1) % SORT_COUNT);
            a->reverse = false;
            app_rebuild(a, m);
            break;
        case 'S':
            a->reverse = !a->reverse;
            app_rebuild(a, m);
            break;
        case 'x': return signal_request(a, m, false);
        case 'X': return signal_request(a, m, true);
        case 'r': {
            const entry *e = app_selected(a, m);
            if (e && e->kind == ENT_CONTAINER)
                ask(a, m, e, e->running ? ACT_DOCKER_RESTART : ACT_DOCKER_START);
            else if (e)
                app_status(a, LVL_INFO, "restart works on containers; processes can only be killed");
            break;
        }
        case 'R': act.type = ACT_REFRESH; return act;
        case '?': a->ovl = OVL_HELP; break;
        default: break;
        }
        break;
    default: break;
    }
    return none;
}
