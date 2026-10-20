#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "screen.h"
#include "util.h"

#define MIN_W 60
#define MIN_H 12

/* Glyphs */
#define G_BAR_L  0x258C /* ▌ */
#define G_HLINE  0x2500 /* ─ */
#define G_TRI_LR 0x25E2 /* ◢ */
#define G_TRI_UL 0x25E4 /* ◤ */
#define G_DOT    0x00B7 /* · */

static const theme *T;
static rgb BASE, ALT;
static bool MONO;

static const uint32_t SPARK[9] = {' ', 0x2581, 0x2582, 0x2583, 0x2584, 0x2585, 0x2586, 0x2587, 0x2588};

static int put(int x, int y, int maxw, const char *s, rgb fg, rgb bg, uint8_t attr) {
    if (maxw <= 0) return 0;
    return scr_text(x, y, maxw, s, fg, bg, attr);
}

/* Truncates s in place to at most w columns, ending with an ellipsis when cut. */
static void fit(char *s, int w) {
    if (w <= 0) {
        s[0] = '\0';
        return;
    }
    if (text_width(s) <= w) return;
    int col = 0;
    char *p = s;
    while (*p && col < w - 1) {
        uint32_t cp;
        int n = utf8_decode(p, &cp);
        if (cp_width(cp) != 0) col++;
        p += n;
    }
    strcpy(p, "\xe2\x80\xa6");
}

static int put_right(int xr, int y, int maxw, const char *s, rgb fg, rgb bg, uint8_t attr) {
    int w = imin(text_width(s), maxw);
    return put(xr - w, y, w, s, fg, bg, attr);
}

static rgb load_color(double v) {
    if (v < 2) return T->dim;
    if (v < 25) return T->primary;
    if (v < 75) return T->warn;
    return T->secondary;
}

static rgb mem_color(uint64_t b) {
    const uint64_t mb = 1024ull * 1024ull;
    if (b < 256 * mb) return T->text;
    if (b < 1024 * mb) return T->primary;
    if (b < 4096 * mb) return T->warn;
    return T->secondary;
}

static int spark_level(float v) {
    if (v <= 0.05f) return 0;
    if (v > 100) v = 100;
    int lvl = (int)ceilf(sqrtf(v / 100.0f) * 8.0f);
    return iclamp(lvl, 1, 8);
}

static void spark(int x, int y, int w, const float *hist, int len, rgb bg) {
    for (int i = 0; i < w; i++) {
        int idx = len - w + i;
        if (idx < 0) continue;
        float v = hist[idx];
        int lvl = spark_level(v);
        rgb c = rgb_mix(T->primary, T->secondary, (v > 100 ? 100 : v) / 100.0f);
        scr_set(x + i, y, lvl ? SPARK[lvl] : G_DOT, lvl ? c : T->faint, bg, 0);
    }
}

/* ---------------------------------------------------------------- layout */

typedef struct {
    int badge, name, project, ports, cpu, spark, mem, pid, up;
    int x_badge, x_name, x_project, x_ports, x_cpu, x_spark, x_mem, x_pid, x_up;
} cols;

static int fixed_width(const cols *c) {
    int w = 1 + c->badge + 1; /* marker, badge, gap */
    int parts[] = {c->project, c->ports, c->cpu, c->spark, c->mem, c->pid, c->up};
    for (size_t i = 0; i < ARRAY_LEN(parts); i++)
        if (parts[i]) w += parts[i] + 1;
    return w + 1; /* right margin */
}

static void layout(int W, cols *c) {
    memset(c, 0, sizeof *c);
    c->badge = 6;
    c->project = 16;
    c->ports = 16;
    c->cpu = 6;
    c->spark = 10;
    c->mem = 7;
    c->pid = 9;
    c->up = 7;
    if (W - fixed_width(c) < 20) c->up = 0;
    if (W - fixed_width(c) < 20) c->spark = 0;
    if (W - fixed_width(c) < 20) c->project = 10;
    if (W - fixed_width(c) < 18) c->pid = 0;
    if (W - fixed_width(c) < 16) c->project = 0;
    if (W - fixed_width(c) < 14) c->ports = 10;
    int extra = W - fixed_width(c) - 44;
    if (extra > 0) {
        int add = imin(extra, 8);
        if (c->project) c->project += add, extra -= add;
        add = imin(extra, 10);
        c->ports += add, extra -= add;
        add = imin(extra, 14);
        if (c->spark) c->spark += add, extra -= add;
    }
    c->name = imax(4, W - fixed_width(c));
    int x = 1;
    c->x_badge = x;
    x += c->badge + 1;
    c->x_name = x;
    x += c->name + 1;
#define PLACE(f)                                                                                                       \
    if (c->f) {                                                                                                        \
        c->x_##f = x;                                                                                                  \
        x += c->f + 1;                                                                                                 \
    }
    PLACE(project)
    PLACE(ports)
    PLACE(cpu)
    PLACE(spark)
    PLACE(mem)
    PLACE(pid)
    PLACE(up)
#undef PLACE
}

/* ---------------------------------------------------------------- header */

static void draw_logo(int y, uint64_t now) {
    static const uint32_t noise[] = {0x2593, 0x2592, 0x2591, '#', '%', '&', '@', '$', '/', '\\'};
    const char *word = "NeonPX";
    /* A short glitch burst every few seconds. */
    bool glitch = (now % 5300) < 160 || ((now % 5300) > 300 && (now % 5300) < 360);
    int x = 1;
    scr_text(x, y, 3, "\xe2\x96\x93\xe2\x96\x92\xe2\x96\x91", T->secondary, RGB_KEEP, 0);
    x += 4;
    int shift = glitch ? (int)((now / 37) % 3) - 1 : 0;
    for (int i = 0; i < 6; i++) {
        uint32_t ch = (uint32_t)word[i];
        rgb c = rgb_mix(T->primary, T->secondary, (float)i / 5.0f);
        if (glitch && ((now / 29 + (uint64_t)i * 7) % 4 == 0)) {
            ch = noise[(now / 13 + (uint64_t)i) % ARRAY_LEN(noise)];
            c = (i % 2) ? T->primary : T->secondary;
        }
        scr_set(x + i * 2 + shift, y, ch, c, RGB_KEEP, ATTR_BOLD);
        if (glitch && i == 2) scr_set(x + i * 2 + 1, y, (uint32_t)word[i], T->primary, RGB_KEEP, 0);
    }
    x += 12;
    scr_text(x, y, 3, "\xe2\x96\x91\xe2\x96\x92\xe2\x96\x93", T->secondary, RGB_KEEP, 0);
}

static void draw_header(const model *m, const ui_opts *o, int W, uint64_t now) {
    for (int x = 0; x < W; x++)
        scr_set(x, 0, ' ', T->text, rgb_mix(T->head_a, T->head_b, (float)x / (float)W), 0);
    draw_logo(0, now);
    int left_end = 21;
    if (W >= 90) left_end += put(left_end, 0, 12, o->version, T->dim, RGB_KEEP, 0) + 1;

    int xr = W - 1;
    char buf[96];
    time_t tt = time(NULL);
    struct tm tmv;
    localtime_r(&tt, &tmv);
    strftime(buf, sizeof buf, "%H:%M:%S", &tmv);
    xr -= put_right(xr, 0, 8, buf, T->primary, RGB_KEEP, ATTR_BOLD) + 3;

    /* DOCKER */
    if (m->docker != DOCKER_DISABLED) {
        const char *dot;
        rgb dc;
        switch (m->docker) {
        case DOCKER_ONLINE:
            dot = "\xe2\x97\x8f";
            dc = T->ok;
            if (W >= 130)
                snprintf(buf, sizeof buf, "%s %zu up", m->docker_runtime, m->containers_running);
            else
                snprintf(buf, sizeof buf, "%zu up", m->containers_running);
            break;
        case DOCKER_CONNECTING:
            dot = "\xe2\x97\x8c";
            dc = T->warn;
            snprintf(buf, sizeof buf, "linking");
            break;
        default:
            dot = "\xe2\x97\x8b";
            dc = T->danger;
            snprintf(buf, sizeof buf, "offline");
            break;
        }
        int w = 7 + 2 + text_width(buf);
        if (xr - w > left_end + 2) {
            int x = xr - w;
            x += put(x, 0, 7, "DOCKER ", T->dim, RGB_KEEP, 0);
            x += put(x, 0, 2, dot, dc, RGB_KEEP, ATTR_BOLD) + 1;
            put(x, 0, text_width(buf), buf, T->text, RGB_KEEP, 0);
            xr -= w + 3;
        }
    }

    /* CPU */
    snprintf(buf, sizeof buf, "%3.0f%%", m->cpu_total);
    int sw = W >= 100 ? 12 : 0;
    int w = 4 + (sw ? sw + 1 : 0) + 4;
    if (xr - w > left_end + 2) {
        int x = xr - w;
        x += put(x, 0, 4, "CPU ", T->dim, RGB_KEEP, 0);
        if (sw) {
            spark(x, 0, sw, m->cpu_hist, m->cpu_hist_len, RGB_KEEP);
            x += sw + 1;
        }
        put(x, 0, 4, buf, load_color(m->cpu_total), RGB_KEEP, ATTR_BOLD);
        xr -= w + 3;
    }

    /* MEM */
    if (m->totals.mem_total) {
        char used[16], total[16];
        fmt_bytes(used, sizeof used, m->totals.mem_used);
        fmt_bytes(total, sizeof total, m->totals.mem_total);
        snprintf(buf, sizeof buf, "%s/%s", used, total);
        int bar = W >= 110 ? 10 : 0;
        int mw = 4 + (bar ? bar + 1 : 0) + text_width(buf);
        if (xr - mw > left_end + 2) {
            int x = xr - mw;
            x += put(x, 0, 4, "MEM ", T->dim, RGB_KEEP, 0);
            double frac = (double)m->totals.mem_used / (double)m->totals.mem_total;
            for (int i = 0; i < bar; i++) {
                bool on = (double)i < frac * bar;
                rgb c = rgb_mix(T->primary, T->secondary, (float)i / (float)(bar ? bar : 1));
                scr_set(x + i, 0, on ? 0x2588 : 0x2591, on ? c : T->faint, RGB_KEEP, 0);
            }
            x += bar ? bar + 1 : 0;
            put(x, 0, text_width(buf), buf, T->text, RGB_KEEP, 0);
        }
    }
}

/* ------------------------------------------------------------------ tabs */

static bool sort_desc_default(sort_key k) { return k == SORT_CPU || k == SORT_MEM || k == SORT_UPTIME; }

static void draw_tabs(const app *a, int W, uint64_t now) {
    int y = 1, x = 1;
    char lbl[32];
    for (int md = 0; md < MODE_COUNT; md++) {
        snprintf(lbl, sizeof lbl, " %s %zu ", mode_name((view_mode)md), a->counts[md]);
        int lw = text_width(lbl);
        if (md == (int)a->mode) {
            scr_set(x++, y, G_TRI_LR, T->secondary, BASE, 0);
            put(x, y, lw, lbl, MONO ? RGB_DEFAULT : T->bg, T->secondary, ATTR_BOLD | (MONO ? ATTR_REVERSE : 0));
            x += lw;
            scr_set(x++, y, G_TRI_UL, T->secondary, BASE, 0);
        } else {
            x++;
            put(x, y, lw, lbl, T->dim, BASE, 0);
            x += lw + 1;
        }
    }
    x += 2;

    char sortlbl[32];
    bool desc = sort_desc_default(a->sort) != a->reverse;
    snprintf(sortlbl, sizeof sortlbl, "%s %s", sort_name(a->sort), desc ? "\xe2\x96\xbc" : "\xe2\x96\xb2");
    int sortw = 6 + text_width(sortlbl);
    int right = W - 1 - sortw;
    if (right > x + 10) {
        put(right, y, 6, "SORT \xe2\x96\xb8", T->dim, BASE, 0);
        put(right + 7, y, text_width(sortlbl), sortlbl, T->primary, BASE, ATTR_BOLD);
    } else {
        right = W - 1;
    }

    int room = right - x - 2;
    if (room < 10) return;
    if (a->editing || a->filter[0]) {
        x += put(x, y, 8, "FILTER \xe2\x96\xb8", a->editing ? T->warn : T->dim, BASE, 0) + 1;
        char f[64];
        str_copy(f, sizeof f, a->filter);
        fit(f, room - 11);
        x += put(x, y, room - 11, f, a->editing ? T->warn : T->primary, BASE, ATTR_BOLD);
        if (a->editing && (now / 500) % 2 == 0) scr_set(x, y, 0x2588, T->warn, BASE, 0);
    } else {
        put(x, y, room, "/ filter", T->faint, BASE, 0);
    }
}

/* ------------------------------------------------------------------ list */

static void column_header(const app *a, const cols *c, int y) {
    rgb fg = T->dim;
    struct {
        int x, w;
        const char *label;
        int sort;
        bool right;
    } hs[] = {
        {c->x_badge, c->badge, "KIND", -1, false},        {c->x_name, c->name, "NAME", SORT_NAME, false},
        {c->x_project, c->project, "PROJECT", -1, false}, {c->x_ports, c->ports, "PORTS", SORT_PORT, false},
        {c->x_cpu, c->cpu, "CPU%", SORT_CPU, true},       {c->x_spark, c->spark, "LOAD", -1, false},
        {c->x_mem, c->mem, "MEM", SORT_MEM, true},        {c->x_pid, c->pid, "PID/ID", SORT_PID, true},
        {c->x_up, c->up, "UP", SORT_UPTIME, true},
    };
    for (size_t i = 0; i < ARRAY_LEN(hs); i++) {
        if (!hs[i].w) continue;
        bool active = hs[i].sort == (int)a->sort;
        rgb col = active ? T->primary : fg;
        uint8_t at = active ? ATTR_BOLD : 0;
        if (hs[i].right)
            put_right(hs[i].x + hs[i].w, y, hs[i].w, hs[i].label, col, BASE, at);
        else
            put(hs[i].x, y, hs[i].w, hs[i].label, col, BASE, at);
    }
}

static void ports_text(const entry *e, char *out, size_t cap) {
    size_t len = 0;
    out[0] = '\0';
    for (int i = 0; i < e->nports; i++) {
        char p[24];
        if (e->kind == ENT_CONTAINER && e->ports[i].target && e->ports[i].target != e->ports[i].port)
            snprintf(p, sizeof p, "%s%u\xe2\x86\x92%u", len ? " " : "", e->ports[i].port, e->ports[i].target);
        else
            snprintf(p, sizeof p, "%s:%u", len ? " " : "", e->ports[i].port);
        str_append(out, cap, &len, p, strlen(p));
    }
    if (e->extra_ports) {
        char p[16];
        snprintf(p, sizeof p, " +%d", e->extra_ports);
        str_append(out, cap, &len, p, strlen(p));
    }
}

static void draw_row(const entry *e, const cols *c, int y, bool sel, int64_t wall, int W, rgb rowbg) {
    scr_fill(0, y, W, 1, ' ', T->text, rowbg);
    uint8_t selattr = (sel && MONO) ? ATTR_REVERSE : 0;
    if (sel) {
        scr_set(0, y, G_BAR_L, T->secondary, rowbg, ATTR_BOLD);
        if (MONO) scr_fill(1, y, W - 1, 1, ' ', T->text, rowbg);
    }
    bool dead = e->kind == ENT_CONTAINER && !e->running;
    rgb text = dead ? T->faint : (sel ? 0xffffff : T->text);

    /* badge */
    rgb rc = T->rt[e->rt];
    char badge[8];
    snprintf(badge, sizeof badge, " %-4s ", runtime_badge(e->rt));
    put(c->x_badge, y, c->badge, badge, dead ? T->faint : rc, MONO ? rowbg : rgb_mix(rc, rowbg, 0.78f),
        ATTR_BOLD | selattr);

    char buf[256];
    str_copy(buf, sizeof buf, e->title);
    fit(buf, c->name);
    int nw = put(c->x_name, y, c->name, buf, text, rowbg, (sel ? ATTR_BOLD : 0) | selattr);
    if (e->rt == RT_PROXY && nw + 8 < c->name) put(c->x_name + nw + 1, y, 7, "[proxy]", T->faint, rowbg, selattr);
    if (dead && nw + 2 < c->name) {
        str_copy(buf, sizeof buf, e->state);
        put(c->x_name + nw + 1, y, c->name - nw - 1, buf, T->faint, rowbg, ATTR_ITALIC | selattr);
    }

    if (c->project) {
        str_copy(buf, sizeof buf, e->project);
        fit(buf, c->project);
        put(c->x_project, y, c->project, buf, dead ? T->faint : T->dim, rowbg, selattr);
    }

    ports_text(e, buf, sizeof buf);
    fit(buf, c->ports);
    put(c->x_ports, y, c->ports, buf, dead ? T->faint : T->primary, rowbg, ATTR_BOLD | selattr);

    if (e->has_cpu && !dead) {
        snprintf(buf, sizeof buf, "%.1f", e->cpu);
        put_right(c->x_cpu + c->cpu, y, c->cpu, buf, load_color(e->cpu), rowbg, ATTR_BOLD | selattr);
    } else {
        put_right(c->x_cpu + c->cpu, y, c->cpu, "\xc2\xb7", T->faint, rowbg, selattr);
    }
    if (c->spark && !dead) spark(c->x_spark, y, c->spark, e->hist, e->hist_len, rowbg);

    if (e->has_mem && !dead) {
        fmt_bytes(buf, sizeof buf, e->mem);
        put_right(c->x_mem + c->mem, y, c->mem, buf, mem_color(e->mem), rowbg, selattr);
    } else {
        put_right(c->x_mem + c->mem, y, c->mem, "\xc2\xb7", T->faint, rowbg, selattr);
    }

    if (c->pid) {
        if (e->kind == ENT_PROC)
            snprintf(buf, sizeof buf, "%d", e->pid);
        else if (e->kind == ENT_CONTAINER)
            str_ncopy(buf, sizeof buf, e->id, 8);
        else
            str_copy(buf, sizeof buf, "-");
        put_right(c->x_pid + c->pid, y, c->pid, buf, T->dim, rowbg, selattr);
    }
    if (c->up && e->start > 0 && e->kind != ENT_PORT) {
        fmt_duration(buf, sizeof buf, wall - e->start);
        put_right(c->x_up + c->up, y, c->up, buf, T->faint, rowbg, selattr);
    }
}

static void draw_empty(const app *a, int y0, int h, int W) {
    if (h < 1) return;
    const char *big = "\xc2\xb7 \xc2\xb7 \xc2\xb7  N O   S I G N A L  \xc2\xb7 \xc2\xb7 \xc2\xb7";
    int y = y0 + h / 2 - 1;
    int bw = text_width(big);
    put((W - bw) / 2, y, bw, big, T->secondary, BASE, ATTR_BOLD);
    char hint[128];
    if (a->filter[0])
        snprintf(hint, sizeof hint, "nothing matches \"%s\"; esc clears the filter", a->filter);
    else if (a->mode != MODE_ALL)
        snprintf(hint, sizeof hint, "tab cycles to ALL for every process");
    else
        snprintf(hint, sizeof hint, "no processes visible");
    int hw = text_width(hint);
    if (y + 2 < y0 + h) put((W - hw) / 2, y + 2, hw, hint, T->dim, BASE, 0);
}

/* --------------------------------------------------------------- details */

static int kv(int x, int y, int maxw, const char *k, const char *v, rgb vc) {
    if (maxw <= 0) return 0;
    int w = put(x, y, maxw, k, T->faint, RGB_KEEP, ATTR_BOLD);
    if (w + 1 >= maxw) return w;
    char buf[600];
    str_copy(buf, sizeof buf, v);
    fit(buf, maxw - w - 1);
    return w + 1 + put(x + w + 1, y, maxw - w - 1, buf, vc, RGB_KEEP, 0);
}

static void listen_text(const entry *e, char *out, size_t cap) {
    size_t len = 0;
    out[0] = '\0';
    for (int i = 0; i < e->nports; i++) {
        char p[80];
        if (e->kind == ENT_CONTAINER)
            snprintf(p, sizeof p, "%s%s:%u\xe2\x86\x92%u", len ? "  " : "", e->ports[i].addr, e->ports[i].port,
                     e->ports[i].target);
        else
            snprintf(p, sizeof p, "%s%s:%u", len ? "  " : "", e->ports[i].addr, e->ports[i].port);
        str_append(out, cap, &len, p, strlen(p));
    }
    if (!len) str_copy(out, cap, "\xe2\x80\x94");
}

static void draw_details(const app *a, const model *m, int y, int h, int W, int64_t wall) {
    for (int x = 0; x < W; x++)
        scr_set(x, y, G_HLINE, T->border, BASE, 0);
    for (int j = 1; j < h; j++)
        scr_fill(0, y + j, W, 1, ' ', T->text, MONO ? BASE : T->panel);
    const entry *e = app_selected(a, m);
    char buf[640], tmp[128];
    int x = 2;
    scr_set(x++, y, 0x2524, T->border, BASE, 0);
    x += put(x, y, 12, " \xe2\x96\xb6 TARGET ", T->secondary, BASE, ATTR_BOLD);
    if (e) {
        str_copy(buf, sizeof buf, e->title);
        fit(buf, W / 2);
        x += put(x, y, W / 2, buf, T->text, BASE, ATTR_BOLD);
        x += put(x, y, 1, " ", T->text, BASE, 0);
    }
    scr_set(x, y, 0x251C, T->border, BASE, 0);
    if (!e || h < 2) return;

    int lx = 2, lw = W - 4, half = lw / 2;
    int ly = y + 1;
    switch (e->kind) {
    case ENT_PROC: {
        kv(lx, ly, lw, "CMD", e->cmd, T->text);
        if (h > 2) {
            path_tilde(buf, sizeof buf, e->cwd[0] ? e->cwd : "\xe2\x80\x94");
            kv(lx, ly + 1, lw - 24, "CWD", buf, T->primary);
            kv(lx + lw - 22, ly + 1, 22, "USER", user_name(e->uid), T->text);
        }
        if (h > 3) {
            const sys_proc *parent = sys_find(m->procs, m->nprocs, e->ppid);
            snprintf(buf, sizeof buf, "%d", e->pid);
            int cx = lx + kv(lx, ly + 2, lw, "PID", buf, T->text) + 2;
            snprintf(buf, sizeof buf, "%d%s%s%s", e->ppid, parent ? " (" : "", parent ? parent->name : "",
                     parent ? ")" : "");
            cx += kv(cx, ly + 2, lx + lw - cx, "PPID", buf, T->text) + 2;
            snprintf(buf, sizeof buf, "%d", e->pgid);
            cx += kv(cx, ly + 2, lx + lw - cx, "PGID", buf, T->text) + 2;
            snprintf(buf, sizeof buf, "%d", e->children);
            cx += kv(cx, ly + 2, lx + lw - cx, "CHILDREN", buf, e->children ? T->warn : T->text) + 2;
            if (e->threads) {
                snprintf(buf, sizeof buf, "%d", e->threads);
                kv(cx, ly + 2, lx + lw - cx, "THREADS", buf, T->text);
            }
        }
        if (h > 4) {
            listen_text(e, buf, sizeof buf);
            kv(lx, ly + 3, lw - 16, "LISTEN", buf, T->primary);
            fmt_duration(tmp, sizeof tmp, wall - e->start);
            kv(lx + lw - 14, ly + 3, 14, "UP", tmp, T->text);
            if (e->rt == RT_PROXY)
                put(lx + half, ly + 3, half - 16, "\xe2\x9a\xa0 docker port proxy: killing it takes docker down",
                    T->warn, RGB_KEEP, 0);
        }
        break;
    }
    case ENT_CONTAINER: {
        int cx = lx + kv(lx, ly, lw - 20, "IMAGE", e->image, T->text) + 2;
        str_ncopy(tmp, sizeof tmp, e->id, 12);
        kv(imax(cx, lx + lw - 18), ly, 18, "ID", tmp, T->dim);
        if (h > 2) {
            cx = lx + kv(lx, ly + 1, lw, "STATE", e->state, e->running ? T->ok : T->danger) + 2;
            put(cx, ly + 1, lx + lw - cx, e->status, T->dim, RGB_KEEP, 0);
        }
        if (h > 3) {
            listen_text(e, buf, sizeof buf);
            kv(lx, ly + 2, lw, "PORTS", buf, T->primary);
        }
        if (h > 4) {
            cx = lx;
            if (e->project[0]) {
                snprintf(buf, sizeof buf, "%s/%s", e->project, e->service);
                cx += kv(cx, ly + 3, half, "COMPOSE", buf, T->text) + 2;
            }
            if (e->has_mem) {
                char used[16], lim[16];
                fmt_bytes(used, sizeof used, e->mem);
                fmt_bytes(lim, sizeof lim, e->mem_limit);
                snprintf(buf, sizeof buf, "%s / %s", used, lim);
                cx += kv(cx, ly + 3, lx + lw - cx, "MEM", buf, T->text) + 2;
                snprintf(buf, sizeof buf, "%d", e->pids);
                kv(cx, ly + 3, lx + lw - cx, "PIDS", buf, T->text);
            }
        }
        break;
    }
    case ENT_PORT:
        snprintf(buf, sizeof buf, "port %u is held by a process this user cannot inspect", e->ports[0].port);
        put(lx, ly, lw, buf, T->text, RGB_KEEP, 0);
        if (h > 2) put(lx, ly + 1, lw, "run `sudo neonpx` to reveal the owner and kill it", T->dim, RGB_KEEP, 0);
        if (h > 3) {
            listen_text(e, buf, sizeof buf);
            kv(lx, ly + 2, lw, "LISTEN", buf, T->primary);
        }
        break;
    }
}

/* ---------------------------------------------------------------- footer */

static void draw_footer(const app *a, int y, int W, uint64_t now, const char *version) {
    scr_fill(0, y, W, 1, ' ', T->text, BASE);
    if (a->status[0] && now < a->status_until) {
        rgb c = a->status_level == LVL_OK     ? T->ok
                : a->status_level == LVL_WARN ? T->warn
                : a->status_level == LVL_ERR  ? T->danger
                                              : T->primary;
        put(1, y, W - 2, a->status, c, BASE, ATTR_BOLD);
        return;
    }
    static const char *const normal[][2] = {
        {"j/k", "move"},  {"/", "filter"},  {"tab", "mode"}, {"s", "sort"}, {"x", "kill"},
        {"X", "kill -9"}, {"r", "restart"}, {"?", "help"},   {"q", "quit"},
    };
    static const char *const editing[][2] = {
        {"type", "to filter"},
        {":5173", "by port"},
        {"enter", "apply"},
        {"esc", "clear"},
    };
    const char *const(*hints)[2] = a->editing ? editing : normal;
    size_t n = a->editing ? ARRAY_LEN(editing) : ARRAY_LEN(normal);
    int x = 1;
    int limit = W - 2 - (W >= 100 ? 16 : 0);
    for (size_t i = 0; i < n; i++) {
        int need = text_width(hints[i][0]) + 1 + text_width(hints[i][1]) + 3;
        if (x + need > limit) break;
        x += put(x, y, 16, hints[i][0], T->secondary, BASE, ATTR_BOLD) + 1;
        x += put(x, y, 16, hints[i][1], T->dim, BASE, 0) + 3;
    }
    if (W >= 100) put_right(W - 1, y, 16, version, T->faint, BASE, 0);
}

/* -------------------------------------------------------------- overlays */

static void box(int x, int y, int w, int h, rgb border) {
    rgb bg = MONO ? BASE : T->panel;
    scr_fill(x, y, w, h, ' ', T->text, bg);
    for (int i = 1; i < w - 1; i++) {
        scr_set(x + i, y, 0x2550, border, bg, 0);
        scr_set(x + i, y + h - 1, 0x2550, border, bg, 0);
    }
    for (int j = 1; j < h - 1; j++) {
        scr_set(x, y + j, 0x2551, border, bg, 0);
        scr_set(x + w - 1, y + j, 0x2551, border, bg, 0);
    }
    scr_set(x, y, 0x2554, border, bg, 0);
    scr_set(x + w - 1, y, 0x2557, border, bg, 0);
    scr_set(x, y + h - 1, 0x255A, border, bg, 0);
    scr_set(x + w - 1, y + h - 1, 0x255D, border, bg, 0);
    /* drop shadow */
    if (!MONO) {
        for (int j = 1; j <= h; j++)
            scr_set(x + w, y + j, ' ', T->text, rgb_mix(BASE, 0x000000, 0.6f), 0);
        for (int i = 1; i <= w; i++)
            scr_set(x + i, y + h, ' ', T->text, rgb_mix(BASE, 0x000000, 0.6f), 0);
    }
}

static void centered(int x, int w, int y, const char *s, rgb fg, uint8_t attr) {
    int tw = imin(text_width(s), w);
    put(x + (w - tw) / 2, y, tw, s, fg, RGB_KEEP, attr);
}

static void draw_confirm(const app *a, int W, int H) {
    const action *p = &a->pending;
    bool hard = p->type == ACT_SIGKILL || p->type == ACT_DOCKER_KILL || p->proxy;
    int w = imin(64, W - 4), h = p->proxy ? 10 : 9;
    int x = (W - w) / 2, y = imax(0, (H - h) / 2);
    rgb border = hard ? T->danger : T->secondary;
    box(x, y, w, h, border);

    const char *title = "", *line2 = "";
    char l2[128];
    switch (p->type) {
    case ACT_SIGTERM:
        title = "\xe2\x9a\xa0  TERMINATE PROCESS";
        line2 = "SIGTERM now, SIGKILL in 3s if it holds on";
        break;
    case ACT_SIGKILL:
        title = "\xe2\x98\xa0  KILL -9";
        line2 = "SIGKILL: no cleanup, no mercy";
        break;
    case ACT_DOCKER_STOP:
        title = "\xe2\x9a\xa0  STOP CONTAINER";
        line2 = "docker stop: SIGTERM, then SIGKILL after 10s";
        break;
    case ACT_DOCKER_KILL:
        title = "\xe2\x98\xa0  KILL CONTAINER";
        line2 = "docker kill: SIGKILL";
        break;
    case ACT_DOCKER_RESTART:
        title = "\xe2\x86\xbb  RESTART CONTAINER";
        line2 = "docker restart";
        break;
    case ACT_DOCKER_START:
        title = "\xe2\x96\xb6  START CONTAINER";
        line2 = "docker start";
        break;
    default: break;
    }
    centered(x, w, y + 1, title, border, ATTR_BOLD);

    char target[160];
    if (p->kind == ENT_PROC)
        snprintf(target, sizeof target, "%s  [%d]", p->label, p->pid);
    else
        snprintf(target, sizeof target, "%s  [%.12s]", p->label, p->id);
    fit(target, w - 4);
    centered(x, w, y + 3, target, T->text, ATTR_BOLD);
    if (p->kind == ENT_PROC) {
        if (p->children)
            snprintf(l2, sizeof l2, "+%d child process%s get%s it too", p->children, p->children == 1 ? "" : "es",
                     p->children == 1 ? "s" : "");
        else
            snprintf(l2, sizeof l2, "no child processes");
        centered(x, w, y + 4, l2, p->children ? T->warn : T->dim, 0);
    }
    centered(x, w, y + 5, line2, T->dim, 0);
    int yb = y + 7;
    if (p->proxy) {
        centered(x, w, y + 6, "\xe2\x9a\xa0 docker's port proxy: killing it takes docker down", T->warn, ATTR_BOLD);
        yb = y + 8;
    }
    int bx = x + (w - 26) / 2;
    put(bx, yb, 3, "[y]", hard ? T->danger : T->ok, RGB_KEEP, ATTR_BOLD);
    put(bx + 4, yb, 7, "execute", T->text, RGB_KEEP, 0);
    put(bx + 16, yb, 3, "[n]", T->dim, RGB_KEEP, ATTR_BOLD);
    put(bx + 20, yb, 5, "abort", T->dim, RGB_KEEP, 0);
}

static void draw_help(int W, int H) {
    static const char *const rows[][2] = {
        {"j k  \xe2\x86\x91 \xe2\x86\x93", "move selection (mouse wheel and click work too)"},
        {"g G  PgUp PgDn", "top / bottom / page"},
        {"tab  1 2 3", "mode: DEV \xc2\xb7 PORTS \xc2\xb7 ALL"},
        {"s  S", "cycle sort column \xc2\xb7 reverse it"},
        {"/", "filter by name, cmd, project, image; :5173 for a port"},
        {"x", "SIGTERM the process tree (SIGKILL after 3s) \xc2\xb7 docker stop"},
        {"X", "SIGKILL the process tree \xc2\xb7 docker kill"},
        {"r", "docker restart \xc2\xb7 start a stopped container"},
        {"R  ctrl-l", "refresh now"},
        {"esc", "clear filter / close"},
        {"q  ctrl-c", "quit"},
        {"", ""},
        {"DEV", "runtimes (node, python, bun...), port holders, containers"},
        {"PORTS", "everything listening on TCP"},
        {"ALL", "every visible process, plus stopped containers"},
    };
    int n = (int)ARRAY_LEN(rows);
    int w = imin(78, W - 4), h = imin(n + 4, H - 2);
    int x = (W - w) / 2, y = imax(0, (H - h) / 2);
    box(x, y, w, h, T->primary);
    centered(x, w, y, " \xe2\x97\xa2 NeonPX // KEYMAP \xe2\x97\xa4 ", T->primary, ATTR_BOLD);
    for (int i = 0; i < n && 2 + i < h - 1; i++) {
        bool mode = i > 11;
        put(x + 3, y + 2 + i, 16, rows[i][0], mode ? T->primary : T->secondary, RGB_KEEP, ATTR_BOLD);
        put(x + 20, y + 2 + i, w - 23, rows[i][1], T->text, RGB_KEEP, 0);
    }
}

/* ---------------------------------------------------------------- render */

void ui_render(app *a, const model *m, const ui_opts *o, uint64_t now) {
    T = o->theme;
    MONO = scr_color_mode() == COLOR_NONE;
    BASE = o->transparent ? RGB_DEFAULT : T->bg;
    ALT = o->transparent ? RGB_DEFAULT : T->bg_alt;
    int W = scr_w(), H = scr_h();
    scr_clear(T->text, BASE);
    int64_t wall = (int64_t)time(NULL);

    if (W < MIN_W || H < MIN_H) {
        const char *msg = "\xe2\x97\xa2 TERMINAL TOO SMALL \xe2\x97\xa4";
        int mw = text_width(msg);
        put(imax(0, (W - mw) / 2), H / 2 - 1, W, msg, T->secondary, BASE, ATTR_BOLD);
        char need[32];
        snprintf(need, sizeof need, "need %dx%d, have %dx%d", MIN_W, MIN_H, W, H);
        mw = text_width(need);
        put(imax(0, (W - mw) / 2), H / 2 + 1, W, need, T->dim, BASE, 0);
        a->list_rows = 1;
        return;
    }

    draw_header(m, o, W, now);
    draw_tabs(a, W, now);

    int detail_h = H >= 24 ? 5 : H >= 18 ? 4 : 0;
    int list_y = 3;
    int list_h = H - 1 - detail_h - list_y;
    a->list_y = list_y;
    a->list_rows = list_h;
    /* Keep the selection on screen after a resize. */
    if (a->nview) {
        if (a->sel < a->top) a->top = a->sel;
        if (a->sel >= a->top + list_h) a->top = a->sel - list_h + 1;
        a->top = iclamp(a->top, 0, imax(0, (int)a->nview - list_h));
    }

    cols c;
    layout(W, &c);
    column_header(a, &c, 2);
    if (!a->nview) draw_empty(a, list_y, list_h, W);
    for (int r = 0; r < list_h; r++) {
        int idx = a->top + r;
        int y = list_y + r;
        rgb rowbg = (r % 2) ? ALT : BASE;
        if (idx >= (int)a->nview) continue;
        bool sel = idx == a->sel;
        draw_row(&m->items[a->view[idx]], &c, y, sel, wall, W, sel && !MONO ? T->sel_bg : rowbg);
    }
    /* scroll indicator */
    if ((int)a->nview > list_h && list_h > 2) {
        int thumb = imax(1, list_h * list_h / (int)a->nview);
        int pos = (list_h - thumb) * a->top / imax(1, (int)a->nview - list_h);
        for (int r = 0; r < list_h; r++)
            scr_set(W - 1, list_y + r, r >= pos && r < pos + thumb ? 0x2503 : 0x2502,
                    r >= pos && r < pos + thumb ? T->secondary : T->faint, RGB_KEEP, 0);
    }

    if (detail_h) draw_details(a, m, H - 1 - detail_h, detail_h, W, wall);
    draw_footer(a, H - 1, W, now, o->version);

    if (a->ovl == OVL_CONFIRM) draw_confirm(a, W, H);
    if (a->ovl == OVL_HELP) draw_help(W, H);
}
