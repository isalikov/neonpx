#ifndef NEONPX_APP_H
#define NEONPX_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "model.h"
#include "term.h"

typedef enum { MODE_DEV, MODE_PORTS, MODE_ALL, MODE_COUNT } view_mode;
typedef enum { SORT_CPU, SORT_MEM, SORT_PORT, SORT_NAME, SORT_PID, SORT_UPTIME, SORT_COUNT } sort_key;
typedef enum { OVL_NONE, OVL_HELP, OVL_CONFIRM } overlay;

typedef enum {
    ACT_NONE,
    ACT_QUIT,
    ACT_REFRESH,
    ACT_SIGTERM,
    ACT_SIGKILL,
    ACT_DOCKER_STOP,
    ACT_DOCKER_KILL,
    ACT_DOCKER_RESTART,
    ACT_DOCKER_START,
} action_type;

/* What to do and to whom: captured when the user asks, so later re-sorting can't retarget it. */
typedef struct {
    action_type type;
    entry_kind kind;
    int pid;
    int64_t start;
    char id[72];
    char label[96];
    int children;
    bool proxy;
} action;

typedef struct {
    view_mode mode;
    sort_key sort;
    bool reverse;
    char filter[64];
    bool editing;

    size_t *view; /* indexes into model.items */
    size_t nview, capview;
    size_t counts[MODE_COUNT];
    int sel, top;
    char sel_key[80];

    overlay ovl;
    action pending;

    char status[192];
    int status_level;
    uint64_t status_until;

    /* Written by the renderer, used for paging and mouse hits. */
    int list_y, list_rows;
    bool quit;
} app;

void app_init(app *a, view_mode mode);
void app_free(app *a);
/* Recomputes the filtered, sorted view and keeps the selection on the same entry. */
void app_rebuild(app *a, const model *m);
action app_key(app *a, const model *m, const key_event *ev);
void app_status(app *a, int level, const char *fmt, ...);
const entry *app_selected(const app *a, const model *m);

bool entry_in_mode(const entry *e, view_mode mode);
bool entry_matches(const entry *e, const char *filter);
const char *sort_name(sort_key k);
const char *mode_name(view_mode m);

#endif
