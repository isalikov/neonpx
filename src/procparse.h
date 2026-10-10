#ifndef NEONPX_PROCPARSE_H
#define NEONPX_PROCPARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Parsers for Linux /proc text formats. Portable so they are unit-tested everywhere. */

typedef struct {
    char name[64];
    char state;
    int ppid, pgid;
    uint64_t utime, stime; /* clock ticks */
    int threads;
    uint64_t starttime; /* clock ticks after boot */
    int64_t rss_pages;
} stat_fields;

int procparse_stat(const char *buf, stat_fields *f);

typedef struct {
    bool v6;
    uint8_t addr[16];
    uint16_t port;
    int state; /* 0x0A = LISTEN */
    uint64_t inode;
} net_entry;

/* One data line of /proc/net/tcp or tcp6. Returns 0 when parsed. */
int procparse_net_line(const char *line, bool v6, net_entry *e);
void procparse_addr(const net_entry *e, char *out, size_t cap);

/* Joins a NUL-separated argv block (as in /proc/pid/cmdline) with spaces. */
void procparse_cmdline(const char *buf, size_t n, char *out, size_t cap);

#endif
