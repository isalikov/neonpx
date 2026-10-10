#ifndef NEONPX_KILL_H
#define NEONPX_KILL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sys.h"

#define KILL_MAX      256
#define KILL_JOBS     8
#define KILL_GRACE_MS 3000

typedef struct {
    int pid;
    int64_t start;
} pid_ref;

/* A signalled process tree, watched until it dies (or escalated to SIGKILL). */
typedef struct {
    bool active;
    pid_ref pids[KILL_MAX];
    int n;
    int sig;
    bool escalated;
    uint64_t deadline_ms;
    char label[96];
} kill_job;

/* Root first, then descendants breadth-first. Returns the count written. */
int kill_collect(const sys_proc *procs, size_t n, int root, pid_ref *out, int cap);

/* Signals pid and its descendants. Returns a LVL_* and writes a status message. */
int kill_start(kill_job *jobs, const sys_proc *procs, size_t n, int pid, int64_t start, int sig, const char *label,
               char *msg, size_t cap);

/* Advances jobs: reports deaths, escalates SIGTERM to SIGKILL after the grace period.
 * Returns a LVL_* when it wrote a message, -1 otherwise. */
int kill_tick(kill_job *jobs, uint64_t now, char *msg, size_t cap);

#endif
