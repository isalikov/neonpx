#include "model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

void model_init(model *m) { memset(m, 0, sizeof *m); }

void model_free(model *m) {
    free(m->items);
    free(m->procs);
    memset(m, 0, sizeof *m);
}

static int key_cmp(const void *a, const void *b) { return strcmp(((const entry *)a)->key, ((const entry *)b)->key); }

const entry *model_find(const model *m, const char *key) {
    size_t lo = 0, hi = m->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(m->items[mid].key, key);
        if (c == 0) return &m->items[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

static int listener_cmp(const void *a, const void *b) {
    const sys_listener *x = a, *y = b;
    if (x->pid != y->pid) return x->pid < y->pid ? -1 : 1;
    if (x->port != y->port) return x->port < y->port ? -1 : 1;
    return (int)x->v6 - (int)y->v6;
}

static void push_hist(float *hist, int *len, float v) {
    if (*len == MODEL_HIST) {
        memmove(hist, hist + 1, sizeof(float) * (MODEL_HIST - 1));
        (*len)--;
    }
    hist[(*len)++] = v;
}

static void add_port(entry *e, uint16_t port, uint16_t target, const char *addr) {
    for (int i = 0; i < e->nports; i++) {
        if (e->ports[i].port == port && e->ports[i].target == target) {
            /* The same port on v4 and v6: a wildcard bind wins for display. */
            if (strcmp(addr, "*") == 0) str_copy(e->ports[i].addr, sizeof e->ports[i].addr, "*");
            return;
        }
    }
    if (e->nports == ENTRY_MAX_PORTS) {
        e->extra_ports++;
        return;
    }
    entry_port *p = &e->ports[e->nports++];
    p->port = port;
    p->target = target;
    str_copy(p->addr, sizeof p->addr, addr);
}

typedef struct {
    uint16_t *ports;
    size_t n;
} port_set;

static int u16_cmp(const void *a, const void *b) {
    uint16_t x = *(const uint16_t *)a, y = *(const uint16_t *)b;
    return (x > y) - (x < y);
}

static bool port_claimed(const port_set *s, uint16_t port) {
    return s->n && bsearch(&port, s->ports, s->n, sizeof *s->ports, u16_cmp) != NULL;
}

static void fill_proc_details(model *m, sys_proc *procs, size_t n) {
    for (size_t i = 0; i < n; i++) {
        sys_proc *p = &procs[i];
        const sys_proc *prev = sys_find(m->procs, m->nprocs, p->pid);
        /* argv and cwd are cached per (pid, start, name); an exec changes the name. */
        if (prev && prev->details && prev->start == p->start && strcmp(prev->name, p->name) == 0) {
            memcpy(p->cmd, prev->cmd, sizeof p->cmd);
            memcpy(p->cwd, prev->cwd, sizeof p->cwd);
            p->details = true;
        } else {
            sys_proc_details(p);
        }
    }
}

static void project_of(const char *cwd, char *out, size_t cap) {
    out[0] = '\0';
    if (!cwd[0]) return;
    const char *home = getenv("HOME");
    if (home && strcmp(cwd, home) == 0)
        str_copy(out, cap, "~");
    else if (strcmp(cwd, "/") == 0)
        str_copy(out, cap, "/");
    else
        str_copy(out, cap, path_base(cwd));
}

void model_refresh(model *m, const docker_snapshot *dk) {
    uint64_t t = now_ns();
    sys_proc *procs = NULL;
    size_t np = 0;
    sys_list_procs(&procs, &np);
    fill_proc_details(m, procs, np);

    sys_listener *ls = NULL;
    size_t nl = 0;
    sys_list_listeners(procs, np, &ls, &nl);
    qsort(ls, nl, sizeof *ls, listener_cmp);

    sys_totals tot;
    sys_read_totals(&tot);

    /* Host ports published by containers. */
    port_set claimed = {0};
    size_t ncont = dk ? dk->n : 0;
    if (ncont) {
        claimed.ports = xmalloc(ncont * DOCKER_MAX_PORTS * sizeof(uint16_t));
        for (size_t i = 0; i < ncont; i++)
            for (int j = 0; j < dk->items[i].nports; j++)
                if (dk->items[i].ports[j].public_port && strcmp(dk->items[i].state, "running") == 0)
                    claimed.ports[claimed.n++] = dk->items[i].ports[j].public_port;
        qsort(claimed.ports, claimed.n, sizeof(uint16_t), u16_cmp);
    }

    size_t cap = np + ncont + nl + 1, running = 0;
    entry *items = xcalloc(cap, sizeof *items);
    size_t k = 0;
    double dt = m->sampled_ns ? (double)(t - m->sampled_ns) : 0;

    size_t li = 0;
    while (li < nl && ls[li].pid == 0)
        li++; /* ownerless listeners sort first */
    for (size_t i = 0; i < np; i++) {
        const sys_proc *p = &procs[i];
        entry *e = &items[k++];
        e->kind = ENT_PROC;
        e->pid = p->pid;
        e->ppid = p->ppid;
        e->pgid = p->pgid;
        e->uid = p->uid;
        e->start = p->start;
        e->threads = p->threads;
        e->running = true;
        snprintf(e->key, sizeof e->key, "p%d:%lld", p->pid, (long long)p->start);
        str_copy(e->name, sizeof e->name, p->name);
        str_copy(e->cmd, sizeof e->cmd, p->cmd);
        str_copy(e->cwd, sizeof e->cwd, p->cwd);
        project_of(p->cwd, e->project, sizeof e->project);
        e->rt = classify(p->name, p->cmd);
        derive_title(e->rt, p->name, p->cmd, e->title, sizeof e->title);
        if (p->has_usage) {
            e->has_mem = true;
            e->mem = p->rss;
            const sys_proc *prev = sys_find(m->procs, m->nprocs, p->pid);
            if (dt > 0 && prev && prev->has_usage && prev->start == p->start && p->cpu_ns >= prev->cpu_ns) {
                e->has_cpu = true;
                e->cpu = (double)(p->cpu_ns - prev->cpu_ns) / dt * 100.0;
            }
        }
        while (li < nl && ls[li].pid < p->pid)
            li++;
        for (; li < nl && ls[li].pid == p->pid; li++) {
            /* Docker's port forwarder: the container row owns these ports. */
            if (e->rt == RT_PROXY && port_claimed(&claimed, ls[li].port)) continue;
            add_port(e, ls[li].port, 0, ls[li].addr);
        }
        /* items[i] mirrors procs[i] in this loop, so the parent's entry shares its index. */
        const sys_proc *parent = sys_find(procs, np, p->ppid);
        if (parent && parent != p) items[parent - procs].children++;
    }

    for (size_t i = 0; i < ncont; i++) {
        const docker_container *c = &dk->items[i];
        entry *e = &items[k++];
        e->kind = ENT_CONTAINER;
        e->rt = RT_DOCKER;
        snprintf(e->key, sizeof e->key, "d%s", c->id);
        str_copy(e->id, sizeof e->id, c->id);
        str_copy(e->title, sizeof e->title, c->name);
        str_copy(e->name, sizeof e->name, c->name);
        str_copy(e->project, sizeof e->project, c->project);
        str_copy(e->service, sizeof e->service, c->service);
        str_copy(e->image, sizeof e->image, c->image);
        str_copy(e->state, sizeof e->state, c->state);
        str_copy(e->status, sizeof e->status, c->status);
        str_copy(e->cmd, sizeof e->cmd, c->image);
        e->start = c->created;
        e->running = strcmp(c->state, "running") == 0;
        e->has_cpu = c->has_cpu;
        e->cpu = c->cpu;
        e->has_mem = c->has_mem;
        e->mem = c->mem;
        e->mem_limit = c->mem_limit;
        e->pids = c->pids;
        for (int j = 0; j < c->nports; j++)
            if (c->ports[j].public_port)
                add_port(e, c->ports[j].public_port, c->ports[j].private_port,
                         c->ports[j].ip[0] ? c->ports[j].ip : "*");
        if (e->running) running++;
    }

    /* Listening ports whose owner we can't see (another user's process, needs root). */
    for (size_t i = 0; i < nl && ls[i].pid == 0; i++) {
        if (port_claimed(&claimed, ls[i].port)) continue;
        if (k && items[k - 1].kind == ENT_PORT && items[k - 1].ports[0].port == ls[i].port) {
            add_port(&items[k - 1], ls[i].port, 0, ls[i].addr);
            continue;
        }
        entry *e = &items[k++];
        e->kind = ENT_PORT;
        e->rt = RT_UNKNOWN;
        e->running = true;
        snprintf(e->key, sizeof e->key, "o%05u", ls[i].port);
        snprintf(e->title, sizeof e->title, "port %u owner hidden", ls[i].port);
        str_copy(e->name, sizeof e->name, "?");
        add_port(e, ls[i].port, 0, ls[i].addr);
    }

    qsort(items, k, sizeof *items, key_cmp);
    for (size_t i = 0; i < k; i++) {
        entry *e = &items[i];
        const entry *prev = model_find(m, e->key);
        if (prev) {
            memcpy(e->hist, prev->hist, sizeof e->hist);
            e->hist_len = prev->hist_len;
        }
        if (e->has_cpu) push_hist(e->hist, &e->hist_len, (float)e->cpu);
    }

    if (m->totals.total && tot.total > m->totals.total && tot.busy >= m->totals.busy) {
        m->cpu_total = (double)(tot.busy - m->totals.busy) / (double)(tot.total - m->totals.total) * 100.0;
        push_hist(m->cpu_hist, &m->cpu_hist_len, (float)m->cpu_total);
    }

    free(m->items);
    free(m->procs);
    free(ls);
    free(claimed.ports);
    m->items = items;
    m->n = k;
    m->procs = procs;
    m->nprocs = np;
    m->totals = tot;
    m->sampled_ns = t;
    m->containers_running = running;
    if (dk) {
        m->docker = dk->state;
        str_copy(m->docker_runtime, sizeof m->docker_runtime, dk->runtime);
        str_copy(m->docker_error, sizeof m->docker_error, dk->error);
    } else {
        m->docker = DOCKER_DISABLED;
    }
}
