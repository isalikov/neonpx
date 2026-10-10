#ifndef NEONPX_SYS_H
#define NEONPX_SYS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Platform layer: sys_darwin.c (libproc) and sys_linux.c (/proc). */

typedef struct {
    int pid, ppid, pgid;
    unsigned uid;
    int64_t start;   /* unix seconds; (pid, start) identifies a process across samples */
    uint64_t cpu_ns; /* cumulative user + system */
    uint64_t rss;    /* bytes */
    int threads;
    bool has_usage; /* cpu_ns/rss could be read */
    bool details;   /* cmd/cwd were fetched */
    char name[64];
    char cmd[512];
    char cwd[256];
} sys_proc;

typedef struct {
    int pid; /* 0 when the owning process is not visible to us */
    uint16_t port;
    bool v6;
    char addr[46];
} sys_listener;

typedef struct {
    uint64_t busy, total; /* cumulative CPU ticks, all cores */
    uint64_t mem_used, mem_total;
    int ncpu;
} sys_totals;

/* Lists processes with basic fields, sorted by pid. Caller frees *out. */
int sys_list_procs(sys_proc **out, size_t *n);
/* Fills cmd and cwd (best effort). */
void sys_proc_details(sys_proc *p);
/* Listening TCP sockets. `procs` must be sorted by pid. Caller frees *out. */
int sys_list_listeners(const sys_proc *procs, size_t n, sys_listener **out, size_t *nout);
/* Start time of a live process, or -1 if it is gone or a zombie. */
int64_t sys_pid_start(int pid);
void sys_read_totals(sys_totals *t);

const sys_proc *sys_find(const sys_proc *procs, size_t n, int pid);

#endif
