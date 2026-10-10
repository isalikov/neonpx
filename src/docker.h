#ifndef NEONPX_DOCKER_H
#define NEONPX_DOCKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DOCKER_MAX_PORTS 16

typedef enum { DOCKER_DISABLED, DOCKER_CONNECTING, DOCKER_ONLINE, DOCKER_OFFLINE } docker_state;
typedef enum { DOCKER_OP_STOP, DOCKER_OP_KILL, DOCKER_OP_RESTART, DOCKER_OP_START } docker_op;

typedef struct {
    uint16_t public_port, private_port;
    char ip[46];
    char proto[8];
} docker_port;

typedef struct {
    char id[72];
    char name[96];
    char image[128];
    char state[16];
    char status[64];
    char project[64]; /* com.docker.compose.project */
    char service[64]; /* com.docker.compose.service */
    docker_port ports[DOCKER_MAX_PORTS];
    int nports;
    int64_t created;
    bool has_cpu;
    double cpu; /* percent of one core */
    bool has_mem;
    uint64_t mem, mem_limit;
    int pids;
} docker_container;

typedef struct {
    docker_container *items;
    size_t n;
    docker_state state;
    char runtime[32]; /* "Docker Desktop", "OrbStack", ... */
    char socket[256];
    char error[128];
    uint64_t generation;
} docker_snapshot;

typedef struct {
    uint64_t cpu_total, system_total;
    int online_cpus;
    bool has_system;
    uint64_t mem_usage, mem_limit;
    int pids;
} docker_stats;

/* Synchronous client; the background poller and `--list` both use it. */
typedef struct docker_client docker_client;
docker_client *docker_client_new(void);
void docker_client_free(docker_client *c);
int docker_client_fetch(docker_client *c, docker_snapshot *out);
int docker_client_op(docker_client *c, docker_op op, const char *id, char *err, size_t cap);

/* Background poller thread. */
void docker_start(void);
void docker_shutdown(void);
void docker_snapshot_get(docker_snapshot *out);
void docker_snapshot_free(docker_snapshot *s);
void docker_submit(docker_op op, const char *id, const char *label);
bool docker_poll_message(char *buf, size_t cap, int *level);

/* Pure helpers, exposed for tests. */
int docker_parse_containers(const char *body, size_t n, docker_container **out, size_t *count);
int docker_parse_stats(const char *body, size_t n, docker_stats *s);
double docker_cpu_percent(const docker_stats *prev, const docker_stats *cur);
const char *docker_runtime_name(const char *socket_path);

#endif
