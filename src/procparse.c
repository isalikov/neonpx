#include "procparse.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "util.h"

int procparse_stat(const char *buf, stat_fields *f) {
    memset(f, 0, sizeof *f);
    /* The command name sits in parentheses and may itself contain ") ". */
    const char *open = strchr(buf, '(');
    const char *close = strrchr(buf, ')');
    if (!open || !close || close < open) return -1;
    str_ncopy(f->name, sizeof f->name, open + 1, (size_t)(close - open - 1));
    unsigned long long utime, stime, start;
    long long rss;
    int n = sscanf(close + 2, "%c %d %d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %*d %*d %*d %*d %d %*d %llu %*u %lld",
                   &f->state, &f->ppid, &f->pgid, &utime, &stime, &f->threads, &start, &rss);
    if (n != 8) return -1;
    f->utime = utime;
    f->stime = stime;
    f->starttime = start;
    f->rss_pages = rss;
    return 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int procparse_net_line(const char *line, bool v6, net_entry *e) {
    memset(e, 0, sizeof *e);
    char hex[40];
    unsigned port, state;
    unsigned long long inode;
    int n = sscanf(line, " %*d: %39[0-9A-Fa-f]:%x %*[0-9A-Fa-f]:%*x %x %*x:%*x %*x:%*x %*x %*u %*u %llu", hex, &port,
                   &state, &inode);
    if (n != 4) return -1;
    size_t words = v6 ? 4 : 1;
    if (strlen(hex) != words * 8) return -1;
    /* Each 32-bit word is printed in host order; copying the value back restores the bytes. */
    for (size_t w = 0; w < words; w++) {
        uint32_t v = 0;
        for (int k = 0; k < 8; k++) {
            int h = hexval(hex[w * 8 + (size_t)k]);
            if (h < 0) return -1;
            v = (v << 4) | (uint32_t)h;
        }
        memcpy(e->addr + w * 4, &v, 4);
    }
    e->v6 = v6;
    e->port = (uint16_t)port;
    e->state = (int)state;
    e->inode = inode;
    return 0;
}

void procparse_addr(const net_entry *e, char *out, size_t cap) {
    static const uint8_t zero[16];
    if (memcmp(e->addr, zero, e->v6 ? 16 : 4) == 0) {
        str_copy(out, cap, "*");
        return;
    }
    char tmp[INET6_ADDRSTRLEN];
    if (!inet_ntop(e->v6 ? AF_INET6 : AF_INET, e->addr, tmp, sizeof tmp)) tmp[0] = '\0';
    str_copy(out, cap, tmp);
}

void procparse_cmdline(const char *buf, size_t n, char *out, size_t cap) {
    size_t len = 0;
    if (cap) out[0] = '\0';
    while (n && buf[n - 1] == '\0')
        n--;
    for (size_t i = 0; i < n;) {
        size_t l = strnlen(buf + i, n - i);
        if (len) str_append(out, cap, &len, " ", 1);
        str_append(out, cap, &len, buf + i, l);
        i += l + 1;
    }
}
