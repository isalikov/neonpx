#include "kill.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

int kill_collect(const sys_proc *procs, size_t n, int root, pid_ref *out, int cap) {
    const sys_proc *r = sys_find(procs, n, root);
    if (!r || cap <= 0) return 0;
    int cnt = 0;
    out[cnt++] = (pid_ref){r->pid, r->start};
    for (int head = 0; head < cnt; head++) {
        int parent = out[head].pid;
        for (size_t i = 0; i < n && cnt < cap; i++) {
            if (procs[i].ppid != parent || procs[i].pid == parent) continue;
            bool seen = false;
            for (int k = 0; k < cnt && !seen; k++)
                seen = out[k].pid == procs[i].pid;
            if (!seen) out[cnt++] = (pid_ref){procs[i].pid, procs[i].start};
        }
    }
    return cnt;
}

static bool alive(const pid_ref *p) {
    int64_t st = sys_pid_start(p->pid);
    return st >= 0 && st == p->start;
}

int kill_start(kill_job *jobs, const sys_proc *procs, size_t n, int pid, int64_t start, int sig, const char *label,
               char *msg, size_t cap) {
    if (pid <= 1) {
        snprintf(msg, cap, "\xe2\x9c\x96 refusing to signal pid %d", pid);
        return LVL_ERR;
    }
    int64_t st = sys_pid_start(pid);
    if (st < 0) {
        snprintf(msg, cap, "%s [%d] is already gone", label, pid);
        return LVL_INFO;
    }
    if (st != start) {
        snprintf(msg, cap, "\xe2\x9c\x96 pid %d was reused by another process, aborted", pid);
        return LVL_ERR;
    }
    pid_ref tree[KILL_MAX];
    int cnt = kill_collect(procs, n, pid, tree, KILL_MAX);
    if (cnt == 0) tree[cnt++] = (pid_ref){pid, start};
    int self = getpid();
    for (int i = 0; i < cnt; i++) {
        if (tree[i].pid == self) {
            snprintf(msg, cap, "\xe2\x9c\x96 refusing: that tree contains neonpx itself");
            return LVL_ERR;
        }
    }

    int sent = 0, denied = 0;
    for (int i = 0; i < cnt; i++) {
        /* Children could have been reaped and recycled since the sample; re-check each. */
        if (i > 0 && !alive(&tree[i])) continue;
        if (kill(tree[i].pid, sig) == 0)
            sent++;
        else if (errno == EPERM)
            denied++;
    }
    const char *signame = sig == SIGKILL ? "SIGKILL" : "SIGTERM";
    if (!sent) {
        if (denied) {
            const sys_proc *p = sys_find(procs, n, pid);
            snprintf(msg, cap, "\xe2\x9c\x96 access denied: %s [%d] belongs to %s, try sudo", label, pid,
                     p ? user_name(p->uid) : "another user");
        } else {
            snprintf(msg, cap, "%s [%d] is already gone", label, pid);
        }
        return LVL_ERR;
    }

    kill_job *job = NULL;
    for (int i = 0; i < KILL_JOBS && !job; i++)
        if (!jobs[i].active) job = &jobs[i];
    if (!job) job = &jobs[0];
    memset(job, 0, sizeof *job);
    job->active = true;
    job->sig = sig;
    job->n = cnt;
    memcpy(job->pids, tree, sizeof tree[0] * (size_t)cnt);
    job->deadline_ms = now_ms() + (sig == SIGKILL ? 1500 : KILL_GRACE_MS);
    str_copy(job->label, sizeof job->label, label);

    if (cnt > 1)
        snprintf(msg, cap, "\xe2\x86\xaf %s \xe2\x86\x92 %s [%d] +%d child%s%s", signame, label, pid, cnt - 1,
                 cnt == 2 ? "" : "ren", denied ? " (some denied)" : "");
    else
        snprintf(msg, cap, "\xe2\x86\xaf %s \xe2\x86\x92 %s [%d]", signame, label, pid);
    return denied ? LVL_WARN : LVL_INFO;
}

int kill_tick(kill_job *jobs, uint64_t now, char *msg, size_t cap) {
    for (int j = 0; j < KILL_JOBS; j++) {
        kill_job *job = &jobs[j];
        if (!job->active) continue;
        int left = 0;
        for (int i = 0; i < job->n; i++)
            if (alive(&job->pids[i])) left++;
        if (left == 0) {
            job->active = false;
            snprintf(msg, cap, "\xe2\x9c\x94 %s [%d] flatlined%s", job->label, job->pids[0].pid,
                     job->escalated ? " (needed SIGKILL)" : "");
            return LVL_OK;
        }
        if (now < job->deadline_ms) continue;
        if (job->sig == SIGTERM) {
            for (int i = 0; i < job->n; i++)
                if (alive(&job->pids[i])) kill(job->pids[i].pid, SIGKILL);
            job->sig = SIGKILL;
            job->escalated = true;
            job->deadline_ms = now + 1500;
            snprintf(msg, cap, "\xe2\x98\xa0 %s ignored SIGTERM, sent SIGKILL to %d process%s", job->label, left,
                     left == 1 ? "" : "es");
            return LVL_WARN;
        }
        job->active = false;
        snprintf(msg, cap, "\xe2\x9c\x96 %s [%d]: %d process%s still alive after SIGKILL", job->label, job->pids[0].pid,
                 left, left == 1 ? "" : "es");
        return LVL_ERR;
    }
    return -1;
}
