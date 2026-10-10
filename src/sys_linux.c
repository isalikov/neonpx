#ifdef __linux__

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "procparse.h"
#include "sys.h"
#include "util.h"

static long clk_tck(void) {
    static long hz;
    if (!hz) hz = sysconf(_SC_CLK_TCK);
    return hz > 0 ? hz : 100;
}

static int64_t boot_time(void) {
    static int64_t btime = -1;
    if (btime >= 0) return btime;
    btime = 0;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        long long v;
        if (sscanf(line, "btime %lld", &v) == 1) {
            btime = v;
            break;
        }
    }
    fclose(f);
    return btime;
}

static ssize_t read_file(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t len = 0;
    while (len + 1 < cap) {
        ssize_t r = read(fd, buf + len, cap - 1 - len);
        if (r <= 0) break;
        len += (size_t)r;
    }
    close(fd);
    buf[len] = '\0';
    return (ssize_t)len;
}

static bool numeric(const char *s) {
    if (!*s) return false;
    for (; *s; s++)
        if (*s < '0' || *s > '9') return false;
    return true;
}

static int pid_cmp(const void *a, const void *b) {
    const sys_proc *x = a, *y = b;
    return (x->pid > y->pid) - (x->pid < y->pid);
}

static bool read_stat(int pid, stat_fields *f) {
    char path[64], buf[1024];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    return read_file(path, buf, sizeof buf) > 0 && procparse_stat(buf, f) == 0;
}

int sys_list_procs(sys_proc **out, size_t *n) {
    DIR *d = opendir("/proc");
    if (!d) {
        *out = NULL;
        *n = 0;
        return -1;
    }
    size_t cap = 512, k = 0;
    sys_proc *procs = xcalloc(cap, sizeof *procs);
    long hz = clk_tck();
    long page = sysconf(_SC_PAGESIZE);
    int64_t btime = boot_time();
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!numeric(de->d_name)) continue;
        int pid = atoi(de->d_name);
        stat_fields f;
        if (!read_stat(pid, &f)) continue;
        /* Skip kernel threads and zombies. */
        if (pid == 2 || f.ppid == 2 || f.state == 'Z') continue;
        if (k == cap) {
            cap *= 2;
            procs = xrealloc(procs, cap * sizeof *procs);
        }
        sys_proc *p = &procs[k++];
        memset(p, 0, sizeof *p);
        p->pid = pid;
        p->ppid = f.ppid;
        p->pgid = f.pgid;
        p->threads = f.threads;
        p->start = btime + (int64_t)(f.starttime / (uint64_t)hz);
        p->cpu_ns = (f.utime + f.stime) * (1000000000ull / (uint64_t)hz);
        p->rss = f.rss_pages > 0 ? (uint64_t)f.rss_pages * (uint64_t)page : 0;
        p->has_usage = true;
        str_copy(p->name, sizeof p->name, f.name);
        char path[64];
        struct stat st;
        snprintf(path, sizeof path, "/proc/%d", pid);
        if (stat(path, &st) == 0) p->uid = st.st_uid;
    }
    closedir(d);
    qsort(procs, k, sizeof *procs, pid_cmp);
    *out = procs;
    *n = k;
    return 0;
}

void sys_proc_details(sys_proc *p) {
    char path[64], buf[4096];
    p->details = true;
    p->cmd[0] = p->cwd[0] = '\0';
    snprintf(path, sizeof path, "/proc/%d/cmdline", p->pid);
    ssize_t n = read_file(path, buf, sizeof buf);
    if (n > 0) procparse_cmdline(buf, (size_t)n, p->cmd, sizeof p->cmd);
    if (!p->cmd[0]) str_copy(p->cmd, sizeof p->cmd, p->name);
    snprintf(path, sizeof path, "/proc/%d/cwd", p->pid);
    ssize_t l = readlink(path, buf, sizeof buf - 1);
    if (l > 0) {
        buf[l] = '\0';
        str_copy(p->cwd, sizeof p->cwd, buf);
    }
}

typedef struct {
    net_entry e;
    bool owned;
} sock_slot;

static int inode_cmp(const void *a, const void *b) {
    const sock_slot *x = a, *y = b;
    return (x->e.inode > y->e.inode) - (x->e.inode < y->e.inode);
}

static void read_net(const char *path, bool v6, sock_slot **socks, size_t *n, size_t *cap) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        net_entry e;
        if (procparse_net_line(line, v6, &e) != 0 || e.state != 0x0A) continue;
        if (*n == *cap) {
            *cap = *cap ? *cap * 2 : 64;
            *socks = xrealloc(*socks, *cap * sizeof **socks);
        }
        (*socks)[*n].e = e;
        (*socks)[*n].owned = false;
        (*n)++;
    }
    fclose(f);
}

static void push(sys_listener **out, size_t *n, size_t *cap, int pid, const net_entry *e) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *out = xrealloc(*out, *cap * sizeof **out);
    }
    sys_listener *l = &(*out)[(*n)++];
    memset(l, 0, sizeof *l);
    l->pid = pid;
    l->port = e->port;
    l->v6 = e->v6;
    procparse_addr(e, l->addr, sizeof l->addr);
}

int sys_list_listeners(const sys_proc *procs, size_t n, sys_listener **out, size_t *nout) {
    *out = NULL;
    *nout = 0;
    size_t cap = 0;
    sock_slot *socks = NULL;
    size_t ns = 0, scap = 0;
    read_net("/proc/net/tcp", false, &socks, &ns, &scap);
    read_net("/proc/net/tcp6", true, &socks, &ns, &scap);
    if (!ns) return 0;
    qsort(socks, ns, sizeof *socks, inode_cmp);

    char path[64], link[64];
    for (size_t i = 0; i < n; i++) {
        snprintf(path, sizeof path, "/proc/%d/fd", procs[i].pid);
        DIR *d = opendir(path);
        if (!d) continue;
        int dfd = dirfd(d);
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            ssize_t l = readlinkat(dfd, de->d_name, link, sizeof link - 1);
            if (l <= 8) continue;
            link[l] = '\0';
            if (strncmp(link, "socket:[", 8) != 0) continue;
            sock_slot key = {.e.inode = strtoull(link + 8, NULL, 10)};
            sock_slot *hit = bsearch(&key, socks, ns, sizeof *socks, inode_cmp);
            if (!hit) continue;
            hit->owned = true;
            push(out, nout, &cap, procs[i].pid, &hit->e);
        }
        closedir(d);
    }
    for (size_t i = 0; i < ns; i++)
        if (!socks[i].owned) push(out, nout, &cap, 0, &socks[i].e);
    free(socks);
    return 0;
}

int64_t sys_pid_start(int pid) {
    stat_fields f;
    if (!read_stat(pid, &f) || f.state == 'Z') return -1;
    return boot_time() + (int64_t)(f.starttime / (uint64_t)clk_tck());
}

void sys_read_totals(sys_totals *t) {
    memset(t, 0, sizeof *t);
    t->ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    char buf[4096];
    if (read_file("/proc/stat", buf, sizeof buf) > 0) {
        unsigned long long v[8] = {0};
        if (sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
                   &v[7]) >= 4) {
            uint64_t idle = v[3] + v[4];
            uint64_t total = 0;
            for (int i = 0; i < 8; i++)
                total += v[i];
            t->total = total;
            t->busy = total - idle;
        }
    }
    if (read_file("/proc/meminfo", buf, sizeof buf) > 0) {
        unsigned long long total = 0, avail = 0;
        const char *p = strstr(buf, "MemTotal:");
        if (p) sscanf(p, "MemTotal: %llu", &total);
        p = strstr(buf, "MemAvailable:");
        if (p) sscanf(p, "MemAvailable: %llu", &avail);
        t->mem_total = total * 1024;
        t->mem_used = total > avail ? (total - avail) * 1024 : 0;
    }
}

#else
typedef int neonpx_sys_linux_unused; /* keep the translation unit non-empty */
#endif
