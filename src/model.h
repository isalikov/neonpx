#ifndef NEONPX_MODEL_H
#define NEONPX_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "classify.h"
#include "docker.h"
#include "sys.h"

#define MODEL_HIST      32
#define ENTRY_MAX_PORTS 12

typedef enum { ENT_PROC, ENT_CONTAINER, ENT_PORT } entry_kind;

typedef struct {
    uint16_t port;   /* host port */
    uint16_t target; /* container port (containers only) */
    char addr[46];
} entry_port;

/* One row: a process, a container, or a listening port with an invisible owner. */
typedef struct {
    entry_kind kind;
    runtime rt;
    char key[80]; /* stable identity across samples */

    int pid, ppid, pgid;
    unsigned uid;
    int64_t start; /* process start / container creation, unix seconds */
    int threads, children;

    char id[72]; /* container id */
    char title[128];
    char name[96];
    char project[64];
    char cmd[512];
    char cwd[256];
    char image[128];
    char state[16];
    char status[64];
    char service[64];

    entry_port ports[ENTRY_MAX_PORTS];
    int nports, extra_ports;

    bool has_cpu;
    double cpu; /* percent of one core */
    bool has_mem;
    uint64_t mem, mem_limit;
    int pids;
    bool running;

    float hist[MODEL_HIST];
    int hist_len;
} entry;

typedef struct {
    entry *items; /* sorted by key */
    size_t n;
    sys_proc *procs; /* sorted by pid */
    size_t nprocs;

    uint64_t sampled_ns;
    sys_totals totals;
    double cpu_total; /* percent of the whole machine */
    float cpu_hist[MODEL_HIST];
    int cpu_hist_len;

    docker_state docker;
    char docker_runtime[32];
    char docker_error[128];
    size_t containers_running;
} model;

void model_init(model *m);
void model_free(model *m);
/* Takes a fresh sample. `dk` may be NULL when Docker is disabled. */
void model_refresh(model *m, const docker_snapshot *dk);
const entry *model_find(const model *m, const char *key);

#endif
