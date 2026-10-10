#include "sys.h"

const sys_proc *sys_find(const sys_proc *procs, size_t n, int pid) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (procs[mid].pid == pid) return &procs[mid];
        if (procs[mid].pid < pid)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}
