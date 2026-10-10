#include "docker.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "http.h"
#include "json.h"
#include "util.h"

/* ---------------------------------------------------------------- parsing */

static void add_port(docker_container *c, const json *p) {
    uint16_t pub = (uint16_t)json_number(json_get(p, "PublicPort"), 0);
    uint16_t priv = (uint16_t)json_number(json_get(p, "PrivatePort"), 0);
    const char *proto = json_string(json_get(p, "Type"), "tcp");
    /* Docker lists 0.0.0.0 and :: bindings separately; keep one row per mapping. */
    for (int i = 0; i < c->nports; i++)
        if (c->ports[i].public_port == pub && c->ports[i].private_port == priv && strcmp(c->ports[i].proto, proto) == 0)
            return;
    if (c->nports >= DOCKER_MAX_PORTS) return;
    docker_port *dp = &c->ports[c->nports++];
    dp->public_port = pub;
    dp->private_port = priv;
    str_copy(dp->ip, sizeof dp->ip, json_string(json_get(p, "IP"), ""));
    str_copy(dp->proto, sizeof dp->proto, proto);
}

static int port_cmp(const void *a, const void *b) {
    const docker_port *x = a, *y = b;
    if ((x->public_port == 0) != (y->public_port == 0)) return x->public_port == 0 ? 1 : -1;
    if (x->public_port != y->public_port) return x->public_port < y->public_port ? -1 : 1;
    return x->private_port < y->private_port ? -1 : x->private_port > y->private_port;
}

int docker_parse_containers(const char *body, size_t n, docker_container **out, size_t *count) {
    json *root = json_parse(body, n);
    if (!root || root->type != JSON_ARR) {
        json_free(root);
        return -1;
    }
    docker_container *items = xcalloc(root->len, sizeof *items);
    size_t k = 0;
    for (size_t i = 0; i < root->len; i++) {
        const json *o = &root->items[i];
        if (o->type != JSON_OBJ) continue;
        docker_container *c = &items[k++];
        str_copy(c->id, sizeof c->id, json_string(json_get(o, "Id"), ""));
        const char *name = json_string(json_at(json_get(o, "Names"), 0), "");
        str_copy(c->name, sizeof c->name, name[0] == '/' ? name + 1 : name);
        if (!c->name[0]) str_ncopy(c->name, sizeof c->name, c->id, 12);
        str_copy(c->image, sizeof c->image, json_string(json_get(o, "Image"), ""));
        str_copy(c->state, sizeof c->state, json_string(json_get(o, "State"), ""));
        str_copy(c->status, sizeof c->status, json_string(json_get(o, "Status"), ""));
        c->created = (int64_t)json_number(json_get(o, "Created"), 0);
        const json *labels = json_get(o, "Labels");
        str_copy(c->project, sizeof c->project, json_string(json_get(labels, "com.docker.compose.project"), ""));
        str_copy(c->service, sizeof c->service, json_string(json_get(labels, "com.docker.compose.service"), ""));
        const json *ports = json_get(o, "Ports");
        for (size_t j = 0; j < json_len(ports); j++)
            add_port(c, json_at(ports, j));
        qsort(c->ports, (size_t)c->nports, sizeof c->ports[0], port_cmp);
    }
    json_free(root);
    *out = items;
    *count = k;
    return 0;
}

int docker_parse_stats(const char *body, size_t n, docker_stats *s) {
    memset(s, 0, sizeof *s);
    json *root = json_parse(body, n);
    if (!root || root->type != JSON_OBJ) {
        json_free(root);
        return -1;
    }
    s->cpu_total = (uint64_t)json_number(json_path(root, "cpu_stats.cpu_usage.total_usage"), 0);
    const json *sys = json_path(root, "cpu_stats.system_cpu_usage");
    s->has_system = sys && sys->type == JSON_NUM;
    s->system_total = (uint64_t)json_number(sys, 0);
    s->online_cpus = (int)json_number(json_path(root, "cpu_stats.online_cpus"), 0);
    if (s->online_cpus <= 0) s->online_cpus = (int)json_len(json_path(root, "cpu_stats.cpu_usage.percpu_usage"));
    if (s->online_cpus <= 0) s->online_cpus = 1;

    double usage = json_number(json_path(root, "memory_stats.usage"), 0);
    /* Match `docker stats`: subtract the reclaimable page cache. cgroup v2 / v1 / legacy. */
    const json *st = json_path(root, "memory_stats.stats");
    double cache = json_number(json_get(st, "inactive_file"), -1);
    if (cache < 0) cache = json_number(json_get(st, "total_inactive_file"), -1);
    if (cache < 0) cache = json_number(json_get(st, "cache"), 0);
    s->mem_usage = usage > cache ? (uint64_t)(usage - cache) : (uint64_t)usage;
    s->mem_limit = (uint64_t)json_number(json_path(root, "memory_stats.limit"), 0);
    s->pids = (int)json_number(json_path(root, "pids_stats.current"), 0);
    json_free(root);
    return 0;
}

double docker_cpu_percent(const docker_stats *prev, const docker_stats *cur) {
    if (!prev->has_system || !cur->has_system) return -1;
    if (cur->system_total <= prev->system_total || cur->cpu_total < prev->cpu_total) return -1;
    double dsys = (double)(cur->system_total - prev->system_total);
    double dcpu = (double)(cur->cpu_total - prev->cpu_total);
    return dcpu / dsys * cur->online_cpus * 100.0;
}

const char *docker_runtime_name(const char *p) {
    if (strstr(p, "/.orbstack/")) return "OrbStack";
    if (strstr(p, "/.colima/")) return "colima";
    if (strstr(p, "/.rd/")) return "Rancher";
    if (strstr(p, "/.lima/")) return "lima";
    if (strstr(p, "podman")) return "podman";
    if (strstr(p, "/.docker/")) return "Docker Desktop";
    return "docker";
}

/* ----------------------------------------------------------------- client */

typedef struct {
    char id[72];
    docker_stats s;
} sample;

struct docker_client {
    char socket[256];
    sample *prev;
    size_t nprev;
};

docker_client *docker_client_new(void) { return xcalloc(1, sizeof(docker_client)); }

void docker_client_free(docker_client *c) {
    if (!c) return;
    free(c->prev);
    free(c);
}

static bool ping(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISSOCK(st.st_mode)) return false;
    int status = 0;
    char *body = NULL;
    size_t n;
    if (http_unix(path, "GET", "/_ping", 1500, &status, &body, &n) != 0) return false;
    free(body);
    return status == 200;
}

static int find_socket(char *out, size_t cap, char *err, size_t errcap) {
    const char *dh = getenv("DOCKER_HOST");
    if (dh && *dh) {
        if (strncmp(dh, "unix://", 7) == 0) {
            if (ping(dh + 7)) {
                str_copy(out, cap, dh + 7);
                return 0;
            }
            snprintf(err, errcap, "DOCKER_HOST %s is not answering", dh);
            return -1;
        }
        snprintf(err, errcap, "DOCKER_HOST %.80s: only unix:// is supported", dh);
        return -1;
    }
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    char cand[16][256];
    int n = 0;
    if (home) {
        snprintf(cand[n++], 256, "%s/.docker/run/docker.sock", home);
        snprintf(cand[n++], 256, "%s/.orbstack/run/docker.sock", home);
        snprintf(cand[n++], 256, "%s/.colima/default/docker.sock", home);
        snprintf(cand[n++], 256, "%s/.colima/docker.sock", home);
        snprintf(cand[n++], 256, "%s/.rd/docker.sock", home);
        snprintf(cand[n++], 256, "%s/.docker/desktop/docker.sock", home);
    }
    snprintf(cand[n++], 256, "/var/run/docker.sock");
    snprintf(cand[n++], 256, "/run/docker.sock");
    if (xdg) {
        snprintf(cand[n++], 256, "%s/docker.sock", xdg);
        snprintf(cand[n++], 256, "%s/podman/podman.sock", xdg);
    }
    snprintf(cand[n++], 256, "/run/user/%u/docker.sock", (unsigned)getuid());
    snprintf(cand[n++], 256, "/run/podman/podman.sock");
    for (int i = 0; i < n; i++) {
        if (ping(cand[i])) {
            str_copy(out, cap, cand[i]);
            return 0;
        }
    }
    snprintf(err, errcap, "no docker socket found");
    return -1;
}

static const sample *find_sample(const sample *s, size_t n, const char *id) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(s[i].id, id) == 0) return &s[i];
    return NULL;
}

int docker_client_fetch(docker_client *c, docker_snapshot *out) {
    memset(out, 0, sizeof *out);
    out->state = DOCKER_OFFLINE;
    if (!c->socket[0] && find_socket(c->socket, sizeof c->socket, out->error, sizeof out->error) != 0) return -1;
    str_copy(out->socket, sizeof out->socket, c->socket);
    str_copy(out->runtime, sizeof out->runtime, docker_runtime_name(c->socket));

    int status = 0;
    char *body = NULL;
    size_t blen = 0;
    if (http_unix(c->socket, "GET", "/containers/json?all=1", 4000, &status, &body, &blen) != 0) {
        snprintf(out->error, sizeof out->error, "%s is not answering", out->runtime);
        c->socket[0] = '\0'; /* rediscover next time: the daemon may have moved */
        return -1;
    }
    if (status != 200 || docker_parse_containers(body, blen, &out->items, &out->n) != 0) {
        snprintf(out->error, sizeof out->error, "docker API error (HTTP %d)", status);
        free(body);
        return -1;
    }
    free(body);

    sample *next = xcalloc(out->n, sizeof *next);
    size_t nnext = 0;
    for (size_t i = 0; i < out->n; i++) {
        docker_container *ct = &out->items[i];
        if (strcmp(ct->state, "running") != 0) continue;
        char path[160];
        snprintf(path, sizeof path, "/containers/%s/stats?stream=false&one-shot=true", ct->id);
        body = NULL;
        if (http_unix(c->socket, "GET", path, 4000, &status, &body, &blen) != 0) continue;
        docker_stats st;
        if (status == 200 && docker_parse_stats(body, blen, &st) == 0) {
            ct->has_mem = true;
            ct->mem = st.mem_usage;
            ct->mem_limit = st.mem_limit;
            ct->pids = st.pids;
            const sample *prev = find_sample(c->prev, c->nprev, ct->id);
            if (prev) {
                double pct = docker_cpu_percent(&prev->s, &st);
                if (pct >= 0) {
                    ct->has_cpu = true;
                    ct->cpu = pct;
                }
            }
            str_copy(next[nnext].id, sizeof next[nnext].id, ct->id);
            next[nnext++].s = st;
        }
        free(body);
    }
    free(c->prev);
    c->prev = next;
    c->nprev = nnext;
    out->state = DOCKER_ONLINE;
    return 0;
}

int docker_client_op(docker_client *c, docker_op op, const char *id, char *err, size_t cap) {
    if (!c->socket[0] && find_socket(c->socket, sizeof c->socket, err, cap) != 0) return -1;
    static const char *const verbs[] = {"stop?t=10", "kill", "restart?t=10", "start"};
    char path[192];
    snprintf(path, sizeof path, "/containers/%s/%s", id, verbs[op]);
    int status = 0;
    char *body = NULL;
    size_t blen = 0;
    if (http_unix(c->socket, "POST", path, 40000, &status, &body, &blen) != 0) {
        snprintf(err, cap, "docker is not answering");
        return -1;
    }
    int rc = 0;
    if (status != 204 && status != 304) {
        json *j = json_parse(body, blen);
        snprintf(err, cap, "%s", json_string(json_get(j, "message"), "docker API error"));
        json_free(j);
        rc = -1;
    }
    free(body);
    return rc;
}

/* ----------------------------------------------------------------- poller */

typedef struct {
    docker_op op;
    char id[72];
    char label[96];
} op_req;

typedef struct {
    char text[256];
    int level;
} message;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread;
    bool running, stop;
    docker_snapshot snap;
    op_req ops[16];
    int nops;
    message msgs[8];
    int nmsgs;
} D = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

static void push_message(int level, const char *text) {
    if (D.nmsgs == (int)ARRAY_LEN(D.msgs)) {
        memmove(D.msgs, D.msgs + 1, sizeof D.msgs[0] * (ARRAY_LEN(D.msgs) - 1));
        D.nmsgs--;
    }
    D.msgs[D.nmsgs].level = level;
    str_copy(D.msgs[D.nmsgs].text, sizeof D.msgs[0].text, text);
    D.nmsgs++;
}

static void *poller(void *arg) {
    (void)arg;
    docker_client *c = docker_client_new();
    uint64_t next_poll = 0;
    pthread_mutex_lock(&D.mu);
    while (!D.stop) {
        if (D.nops) {
            op_req r = D.ops[0];
            memmove(D.ops, D.ops + 1, sizeof D.ops[0] * (size_t)(D.nops - 1));
            D.nops--;
            pthread_mutex_unlock(&D.mu);
            char err[128] = "", text[256];
            int rc = docker_client_op(c, r.op, r.id, err, sizeof err);
            static const char *const done[] = {"stopped", "killed", "restarted", "started"};
            if (rc == 0)
                snprintf(text, sizeof text, "\xe2\x9c\x94 container %s %s", r.label, done[r.op]);
            else
                snprintf(text, sizeof text, "\xe2\x9c\x96 %s: %s", r.label, err);
            pthread_mutex_lock(&D.mu);
            push_message(rc == 0 ? LVL_OK : LVL_ERR, text);
            next_poll = 0;
            continue;
        }
        uint64_t t = now_ms();
        if (t >= next_poll) {
            pthread_mutex_unlock(&D.mu);
            docker_snapshot snap;
            int rc = docker_client_fetch(c, &snap);
            pthread_mutex_lock(&D.mu);
            snap.generation = D.snap.generation + 1;
            docker_snapshot_free(&D.snap);
            D.snap = snap;
            next_poll = now_ms() + (rc == 0 ? 2000 : 5000);
            continue;
        }
        uint64_t wait = next_poll - t;
        struct timeval tv;
        gettimeofday(&tv, NULL);
        uint64_t ns = (uint64_t)tv.tv_usec * 1000ull + wait * 1000000ull;
        struct timespec ts = {tv.tv_sec + (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
        pthread_cond_timedwait(&D.cv, &D.mu, &ts);
    }
    pthread_mutex_unlock(&D.mu);
    docker_client_free(c);
    return NULL;
}

void docker_start(void) {
    pthread_mutex_lock(&D.mu);
    if (!D.running) {
        D.stop = false;
        D.snap.state = DOCKER_CONNECTING;
        if (pthread_create(&D.thread, NULL, poller, NULL) == 0)
            D.running = true;
        else
            D.snap.state = DOCKER_DISABLED;
    }
    pthread_mutex_unlock(&D.mu);
}

void docker_shutdown(void) {
    pthread_mutex_lock(&D.mu);
    if (!D.running) {
        pthread_mutex_unlock(&D.mu);
        return;
    }
    D.stop = true;
    pthread_cond_broadcast(&D.cv);
    pthread_mutex_unlock(&D.mu);
    /* A request in flight is bounded by its socket timeout; don't hold up exit for it. */
    pthread_detach(D.thread);
    D.running = false;
}

void docker_snapshot_get(docker_snapshot *out) {
    pthread_mutex_lock(&D.mu);
    *out = D.snap;
    out->items = NULL;
    if (D.snap.n) {
        out->items = xmalloc(D.snap.n * sizeof *out->items);
        memcpy(out->items, D.snap.items, D.snap.n * sizeof *out->items);
    }
    pthread_mutex_unlock(&D.mu);
}

void docker_snapshot_free(docker_snapshot *s) {
    free(s->items);
    s->items = NULL;
    s->n = 0;
}

void docker_submit(docker_op op, const char *id, const char *label) {
    pthread_mutex_lock(&D.mu);
    if (D.running && D.nops < (int)ARRAY_LEN(D.ops)) {
        op_req *r = &D.ops[D.nops++];
        r->op = op;
        str_copy(r->id, sizeof r->id, id);
        str_copy(r->label, sizeof r->label, label);
        pthread_cond_broadcast(&D.cv);
    }
    pthread_mutex_unlock(&D.mu);
}

bool docker_poll_message(char *buf, size_t cap, int *level) {
    pthread_mutex_lock(&D.mu);
    bool got = D.nmsgs > 0;
    if (got) {
        str_copy(buf, cap, D.msgs[0].text);
        *level = D.msgs[0].level;
        memmove(D.msgs, D.msgs + 1, sizeof D.msgs[0] * (size_t)(D.nmsgs - 1));
        D.nmsgs--;
    }
    pthread_mutex_unlock(&D.mu);
    return got;
}
