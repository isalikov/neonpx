#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "docker.h"
#include "kill.h"
#include "model.h"
#include "screen.h"
#include "term.h"
#include "theme.h"
#include "ui.h"
#include "util.h"
#include "version.h"

typedef struct {
    int interval_ms;
    view_mode mode;
    bool docker;
    bool list;
    bool transparent;
    const char *theme;
} options;

static void usage(FILE *f) {
    fprintf(f,
            "neonpx %s: who holds my ports, and how hard are they burning.\n"
            "\n"
            "Usage: neonpx [options]\n"
            "\n"
            "  -a, --all            start in ALL mode (every process)\n"
            "  -p, --ports          start in PORTS mode (TCP listeners only)\n"
            "  -i, --interval SEC   sampling interval, 0.25-10 (default 1)\n"
            "  -t, --theme NAME     color theme: %s\n"
            "      --no-bg          keep the terminal's own background\n"
            "      --no-docker      do not talk to the Docker daemon\n"
            "  -l, --list           print one snapshot as a table and exit\n"
            "  -V, --version        print version\n"
            "  -h, --help           this help\n"
            "\n"
            "Environment: NEONPX_THEME, NEONPX_COLOR (truecolor|256|16|none), NO_COLOR, DOCKER_HOST.\n"
            "Inside the app press ? for keys.\n",
            NEONPX_VERSION, theme_names());
}

static int parse_args(int argc, char **argv, options *o) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            exit(0);
        } else if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
            printf("neonpx %s\n", NEONPX_VERSION);
            exit(0);
        } else if (!strcmp(a, "-a") || !strcmp(a, "--all")) {
            o->mode = MODE_ALL;
        } else if (!strcmp(a, "-p") || !strcmp(a, "--ports")) {
            o->mode = MODE_PORTS;
        } else if (!strcmp(a, "-l") || !strcmp(a, "--list")) {
            o->list = true;
        } else if (!strcmp(a, "--no-docker")) {
            o->docker = false;
        } else if (!strcmp(a, "--no-bg")) {
            o->transparent = true;
        } else if ((!strcmp(a, "-i") || !strcmp(a, "--interval")) && i + 1 < argc) {
            double s = atof(argv[++i]);
            if (s < 0.25 || s > 10) {
                fprintf(stderr, "neonpx: interval must be between 0.25 and 10 seconds\n");
                return -1;
            }
            o->interval_ms = (int)(s * 1000);
        } else if ((!strcmp(a, "-t") || !strcmp(a, "--theme")) && i + 1 < argc) {
            o->theme = argv[++i];
        } else {
            fprintf(stderr, "neonpx: unknown option '%s'\n\n", a);
            usage(stderr);
            return -1;
        }
    }
    return 0;
}

static void print_list(const options *o) {
    model m;
    model_init(&m);
    docker_client *dc = o->docker ? docker_client_new() : NULL;
    docker_snapshot ds = {0};
    /* Two samples so CPU percentages have something to compare against. */
    for (int pass = 0; pass < 2; pass++) {
        if (dc) {
            docker_snapshot_free(&ds);
            docker_client_fetch(dc, &ds);
        }
        model_refresh(&m, dc ? &ds : NULL);
        if (pass == 0) sleep_ms(o->interval_ms < 1000 ? o->interval_ms : 1000);
    }
    app a;
    app_init(&a, o->mode);
    app_rebuild(&a, &m);
    printf("%-5s %-28s %-16s %-18s %6s %7s %10s\n", "KIND", "NAME", "PROJECT", "PORTS", "CPU%", "MEM", "PID/ID");
    for (size_t i = 0; i < a.nview; i++) {
        const entry *e = &m.items[a.view[i]];
        char ports[128] = "", mem[16] = "-", cpu[16] = "-", id[16];
        size_t len = 0;
        for (int p = 0; p < e->nports; p++) {
            char one[24];
            if (e->kind == ENT_CONTAINER && e->ports[p].target)
                snprintf(one, sizeof one, "%s%u->%u", len ? "," : "", e->ports[p].port, e->ports[p].target);
            else
                snprintf(one, sizeof one, "%s:%u", len ? "," : "", e->ports[p].port);
            str_append(ports, sizeof ports, &len, one, strlen(one));
        }
        if (e->has_mem) fmt_bytes(mem, sizeof mem, e->mem);
        if (e->has_cpu) snprintf(cpu, sizeof cpu, "%.1f", e->cpu);
        if (e->kind == ENT_CONTAINER)
            str_ncopy(id, sizeof id, e->id, 10);
        else
            snprintf(id, sizeof id, "%d", e->pid);
        printf("%-5s %-28.28s %-16.16s %-18.18s %6s %7s %10s\n", runtime_badge(e->rt), e->title, e->project,
               len ? ports : "-", cpu, mem, id);
    }
    if (dc && ds.state != DOCKER_ONLINE && ds.error[0]) fprintf(stderr, "docker: %s\n", ds.error);
    app_free(&a);
    docker_snapshot_free(&ds);
    docker_client_free(dc);
    model_free(&m);
}

static void dispatch(app *a, const model *m, const action *act, kill_job *jobs) {
    char msg[192];
    switch (act->type) {
    case ACT_SIGTERM:
    case ACT_SIGKILL: {
        int sig = act->type == ACT_SIGKILL ? SIGKILL : SIGTERM;
        int lvl = kill_start(jobs, m->procs, m->nprocs, act->pid, act->start, sig, act->label, msg, sizeof msg);
        app_status(a, lvl, "%s", msg);
        break;
    }
    case ACT_DOCKER_STOP:
    case ACT_DOCKER_KILL:
    case ACT_DOCKER_RESTART:
    case ACT_DOCKER_START: {
        static const struct {
            docker_op op;
            const char *verb;
        } ops[] = {
            {DOCKER_OP_STOP, "stopping"},
            {DOCKER_OP_KILL, "killing"},
            {DOCKER_OP_RESTART, "restarting"},
            {DOCKER_OP_START, "starting"},
        };
        int i = act->type - ACT_DOCKER_STOP;
        docker_submit(ops[i].op, act->id, act->label);
        app_status(a, LVL_INFO, "\xc2\xbb %s container %s\xe2\x80\xa6", ops[i].verb, act->label);
        break;
    }
    default: break;
    }
}

int main(int argc, char **argv) {
    options o = {.interval_ms = 1000, .mode = MODE_DEV, .docker = true, .theme = getenv("NEONPX_THEME")};
    if (parse_args(argc, argv, &o) != 0) return 2;
    signal(SIGPIPE, SIG_IGN);
    srand((unsigned)time(NULL));

    if (o.list) {
        print_list(&o);
        return 0;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "neonpx: needs an interactive terminal; use --list for plain output\n");
        return 2;
    }

    if (o.theme && *o.theme && !theme_find(o.theme))
        fprintf(stderr, "neonpx: unknown theme '%s', using neon (available: %s)\n", o.theme, theme_names());
    if (o.docker) docker_start();
    model m;
    model_init(&m);
    app a;
    app_init(&a, o.mode);
    docker_snapshot ds = {0};
    if (o.docker) docker_snapshot_get(&ds);
    model_refresh(&m, o.docker ? &ds : NULL);
    app_rebuild(&a, &m);

    if (term_init() != 0) {
        fprintf(stderr, "neonpx: cannot switch the terminal to raw mode\n");
        return 1;
    }
    scr_init(scr_detect_color());
    int w, h;
    term_size(&w, &h);
    scr_resize(w, h);

    ui_opts uo = {.theme = theme_get(o.theme), .transparent = o.transparent, .version = "v" NEONPX_VERSION};
    kill_job jobs[KILL_JOBS];
    memset(jobs, 0, sizeof jobs);
    uint64_t next_sample = now_ms() + 350; /* quick second sample so CPU% appears fast */
    uint64_t last_docker_gen = 0;

    while (!term_quit_requested()) {
        uint64_t t = now_ms();
        if (o.docker) {
            /* Pick up a fresh docker snapshot as soon as the poller publishes one. */
            docker_snapshot_free(&ds);
            docker_snapshot_get(&ds);
            if (ds.generation != last_docker_gen && t + 200 < next_sample) next_sample = t;
            last_docker_gen = ds.generation;
        }
        if (t >= next_sample) {
            model_refresh(&m, o.docker ? &ds : NULL);
            app_rebuild(&a, &m);
            next_sample = t + (uint64_t)o.interval_ms;
        }
        char msg[192];
        int lvl = kill_tick(jobs, t, msg, sizeof msg);
        if (lvl >= 0) {
            app_status(&a, lvl, "%s", msg);
            next_sample = t; /* show the corpse leaving */
        }
        while (docker_poll_message(msg, sizeof msg, &lvl))
            app_status(&a, lvl, "%s", msg);

        ui_render(&a, &m, &uo, t);
        scr_flush(STDOUT_FILENO);

        int timeout = 100;
        if (next_sample > t && next_sample - t < (uint64_t)timeout) timeout = (int)(next_sample - t);
        key_event ev;
        if (!term_wait(timeout, &ev)) continue;
        if (ev.type == KEY_RESIZE) {
            term_size(&w, &h);
            scr_resize(w, h);
            continue;
        }
        action act = app_key(&a, &m, &ev);
        if (act.type == ACT_QUIT) break;
        if (act.type == ACT_REFRESH) {
            next_sample = 0;
            scr_invalidate();
            continue;
        }
        dispatch(&a, &m, &act, jobs);
    }

    term_restore();
    docker_shutdown();
    docker_snapshot_free(&ds);
    scr_free();
    app_free(&a);
    model_free(&m);
    return 0;
}
