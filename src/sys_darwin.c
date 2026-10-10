#ifdef __APPLE__

#include <arpa/inet.h>
#include <errno.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "sys.h"
#include "util.h"

static int pid_cmp(const void *a, const void *b) {
    const sys_proc *x = a, *y = b;
    return (x->pid > y->pid) - (x->pid < y->pid);
}

static uint64_t ticks_to_ns(uint64_t ticks) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    /* On Apple Silicon task times are mach ticks (125/3 ns), on Intel 1:1. */
    return (uint64_t)((__uint128_t)ticks * tb.numer / tb.denom);
}

static void read_usage(sys_proc *p) {
    struct proc_taskinfo ti;
    if (proc_pidinfo(p->pid, PROC_PIDTASKINFO, 0, &ti, sizeof ti) == (int)sizeof ti) {
        p->cpu_ns = ticks_to_ns(ti.pti_total_user + ti.pti_total_system);
        p->rss = ti.pti_resident_size;
        p->threads = ti.pti_threadnum;
        p->has_usage = true;
    }
}

int sys_list_procs(sys_proc **out, size_t *n) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
    struct kinfo_proc *kp = NULL;
    size_t size = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0) break;
        size += size / 8 + sizeof(struct kinfo_proc) * 16;
        kp = xrealloc(kp, size);
        if (sysctl(mib, 4, kp, &size, NULL, 0) == 0) break;
        if (errno != ENOMEM) {
            size = 0;
            break;
        }
        size = 0;
    }
    size_t count = size / sizeof(struct kinfo_proc);
    if (!count) {
        free(kp);
        *out = NULL;
        *n = 0;
        return -1;
    }
    sys_proc *procs = xcalloc(count, sizeof *procs);
    size_t k = 0;
    for (size_t i = 0; i < count; i++) {
        const struct kinfo_proc *e = &kp[i];
        if (e->kp_proc.p_pid <= 0 || e->kp_proc.p_stat == SZOMB) continue;
        sys_proc *p = &procs[k++];
        p->pid = e->kp_proc.p_pid;
        p->ppid = e->kp_eproc.e_ppid;
        p->pgid = e->kp_eproc.e_pgid;
        p->uid = e->kp_eproc.e_ucred.cr_uid;
        p->start = e->kp_proc.p_starttime.tv_sec;
        if (proc_name(p->pid, p->name, sizeof p->name) <= 0) str_copy(p->name, sizeof p->name, e->kp_proc.p_comm);
        read_usage(p);
    }
    free(kp);
    qsort(procs, k, sizeof *procs, pid_cmp);
    *out = procs;
    *n = k;
    return 0;
}

void sys_proc_details(sys_proc *p) {
    static char *argbuf;
    static size_t argmax;
    p->details = true;
    p->cmd[0] = p->cwd[0] = '\0';
    if (!argmax) {
        int mib[2] = {CTL_KERN, KERN_ARGMAX}, am = 0;
        size_t s = sizeof am;
        argmax = (sysctl(mib, 2, &am, &s, NULL, 0) == 0 && am > 0) ? (size_t)am : 262144;
        argbuf = xmalloc(argmax);
    }
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, p->pid};
    size_t size = argmax;
    if (sysctl(mib, 3, argbuf, &size, NULL, 0) == 0 && size > sizeof(int)) {
        /* Layout: argc, exec path, NUL padding, argv[0..argc), environment. */
        int argc;
        memcpy(&argc, argbuf, sizeof argc);
        const char *q = argbuf + sizeof(int), *end = argbuf + size;
        while (q < end && *q)
            q++;
        while (q < end && !*q)
            q++;
        size_t len = 0;
        for (int i = 0; i < argc && q < end; i++) {
            size_t l = strnlen(q, (size_t)(end - q));
            if (len) str_append(p->cmd, sizeof p->cmd, &len, " ", 1);
            str_append(p->cmd, sizeof p->cmd, &len, q, l);
            q += l + 1;
        }
    }
    if (!p->cmd[0]) str_copy(p->cmd, sizeof p->cmd, p->name);
    struct proc_vnodepathinfo vpi;
    if (proc_pidinfo(p->pid, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof vpi) == (int)sizeof vpi)
        str_copy(p->cwd, sizeof p->cwd, vpi.pvi_cdir.vip_path);
}

static void push_listener(sys_listener **out, size_t *n, size_t *cap, const sys_listener *l) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *out = xrealloc(*out, *cap * sizeof **out);
    }
    (*out)[(*n)++] = *l;
}

int sys_list_listeners(const sys_proc *procs, size_t n, sys_listener **out, size_t *nout) {
    *out = NULL;
    *nout = 0;
    size_t cap = 0;
    uid_t me = geteuid();
    struct proc_fdinfo *fds = NULL;
    size_t fdcap = 0;
    for (size_t i = 0; i < n; i++) {
        const sys_proc *p = &procs[i];
        /* Other users' file tables are off limits unless we are root; skip the syscalls. */
        if (me != 0 && p->uid != me) continue;
        int bytes = proc_pidinfo(p->pid, PROC_PIDLISTFDS, 0, NULL, 0);
        if (bytes <= 0) continue;
        if ((size_t)bytes > fdcap) {
            fdcap = (size_t)bytes * 2;
            fds = xrealloc(fds, fdcap);
        }
        bytes = proc_pidinfo(p->pid, PROC_PIDLISTFDS, 0, fds, (int)fdcap);
        if (bytes <= 0) continue;
        int nfds = bytes / (int)sizeof(struct proc_fdinfo);
        for (int f = 0; f < nfds; f++) {
            if (fds[f].proc_fdtype != PROX_FDTYPE_SOCKET) continue;
            struct socket_fdinfo si;
            if (proc_pidfdinfo(p->pid, fds[f].proc_fd, PROC_PIDFDSOCKETINFO, &si, sizeof si) != (int)sizeof si)
                continue;
            if (si.psi.soi_kind != SOCKINFO_TCP) continue;
            if (si.psi.soi_proto.pri_tcp.tcpsi_state != TSI_S_LISTEN) continue;
            const struct in_sockinfo *in = &si.psi.soi_proto.pri_tcp.tcpsi_ini;
            sys_listener l = {.pid = p->pid, .port = ntohs((uint16_t)in->insi_lport)};
            if (in->insi_vflag & INI_IPV6) {
                l.v6 = true;
                static const struct in6_addr any6;
                if (memcmp(&in->insi_laddr.ina_6, &any6, sizeof any6) == 0)
                    str_copy(l.addr, sizeof l.addr, "*");
                else
                    inet_ntop(AF_INET6, &in->insi_laddr.ina_6, l.addr, sizeof l.addr);
            } else {
                if (in->insi_laddr.ina_46.i46a_addr4.s_addr == INADDR_ANY)
                    str_copy(l.addr, sizeof l.addr, "*");
                else
                    inet_ntop(AF_INET, &in->insi_laddr.ina_46.i46a_addr4, l.addr, sizeof l.addr);
            }
            push_listener(out, nout, &cap, &l);
        }
    }
    free(fds);
    return 0;
}

int64_t sys_pid_start(int pid) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    struct kinfo_proc kp;
    size_t size = sizeof kp;
    if (sysctl(mib, 4, &kp, &size, NULL, 0) != 0 || size == 0) return -1;
    if (kp.kp_proc.p_stat == SZOMB) return -1;
    return kp.kp_proc.p_starttime.tv_sec;
}

void sys_read_totals(sys_totals *t) {
    static mach_port_t host;
    if (!host) host = mach_host_self();
    memset(t, 0, sizeof *t);
    t->ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);

    host_cpu_load_info_data_t cpu = {0};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(host, HOST_CPU_LOAD_INFO, (host_info_t)&cpu, &count) == KERN_SUCCESS) {
        t->busy =
            (uint64_t)cpu.cpu_ticks[CPU_STATE_USER] + cpu.cpu_ticks[CPU_STATE_SYSTEM] + cpu.cpu_ticks[CPU_STATE_NICE];
        t->total = t->busy + cpu.cpu_ticks[CPU_STATE_IDLE];
    }

    uint64_t memsize = 0;
    size_t len = sizeof memsize;
    if (sysctlbyname("hw.memsize", &memsize, &len, NULL, 0) == 0) t->mem_total = memsize;
    vm_statistics64_data_t vm = {0};
    count = HOST_VM_INFO64_COUNT;
    vm_size_t page = 0;
    if (host_page_size(host, &page) == KERN_SUCCESS &&
        host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vm, &count) == KERN_SUCCESS) {
        /* Activity Monitor's "Memory Used": app memory + wired + compressed. */
        uint64_t pages =
            (uint64_t)vm.internal_page_count - vm.purgeable_count + vm.wire_count + vm.compressor_page_count;
        t->mem_used = pages * page;
    }
}

#else
typedef int neonpx_sys_darwin_unused; /* keep the translation unit non-empty */
#endif
