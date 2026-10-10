#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "classify.h"
#include "docker.h"
#include "http.h"
#include "json.h"
#include "kill.h"
#include "model.h"
#include "procparse.h"
#include "screen.h"
#include "term.h"
#include "util.h"

static int failures, checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            failures++;                                                                                                \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                          \
        }                                                                                                              \
    } while (0)

#define CHECK_STR(a, b)                                                                                                \
    do {                                                                                                               \
        checks++;                                                                                                      \
        const char *a_ = (a), *b_ = (b);                                                                               \
        if (strcmp(a_, b_) != 0) {                                                                                     \
            failures++;                                                                                                \
            fprintf(stderr, "  FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, a_, b_);                           \
        }                                                                                                              \
    } while (0)

/* ------------------------------------------------------------------ util */

static void test_util(void) {
    char buf[16];
    str_copy(buf, 5, "abcdef");
    CHECK_STR(buf, "abcd");
    /* "жж" is 4 bytes; a cap of 4 leaves room for 3, which must not split the second letter. */
    str_copy(buf, 4, "\xd0\xb6\xd0\xb6");
    CHECK_STR(buf, "\xd0\xb6");
    CHECK(str_icontains("Next-Server", "next"));
    CHECK(!str_icontains("vite", "next"));
    CHECK(str_istarts("Python3.12", "python"));
    CHECK_STR(path_base("/usr/bin/node"), "node");
    CHECK_STR(path_base("/a/b/proj/"), "proj/");

    fmt_bytes(buf, sizeof buf, 512);
    CHECK_STR(buf, "512B");
    fmt_bytes(buf, sizeof buf, 312ull << 20);
    CHECK_STR(buf, "312M");
    fmt_bytes(buf, sizeof buf, 5ull << 20);
    CHECK_STR(buf, "5.0M");
    fmt_bytes(buf, sizeof buf, 3ull << 30);
    CHECK_STR(buf, "3.0G");
    fmt_duration(buf, sizeof buf, 42);
    CHECK_STR(buf, "42s");
    fmt_duration(buf, sizeof buf, 3 * 60 + 5);
    CHECK_STR(buf, "3m05s");
    fmt_duration(buf, sizeof buf, 2 * 3600 + 13 * 60);
    CHECK_STR(buf, "2h13m");
    fmt_duration(buf, sizeof buf, 3 * 86400 + 4 * 3600);
    CHECK_STR(buf, "3d04h");

    uint32_t cp;
    CHECK(utf8_decode("\xe2\x96\x93", &cp) == 3 && cp == 0x2593);
    CHECK(utf8_decode("\xff", &cp) == 1 && cp == 0xFFFD);
    char enc[4];
    CHECK(utf8_encode(0x1F600, enc) == 4);
    CHECK(cp_width(0x23F3) == 2 && cp_width(0x26A1) == 2 && cp_width(0x2714) == 1 && cp_width(0x1F600) == 2);
    CHECK(cp_width(0x4E2D) == 2 && cp_width('a') == 1 && cp_width(0x301) == 0 && cp_width(7) == -1);
    CHECK(text_width("a\xe2\x96\x93z") == 3);
}

/* ------------------------------------------------------------------ json */

static void test_json(void) {
    const char *src = "{\"a\": [1, 2.5, -3e2], \"s\": \"x\\n\\u00e9\\ud83d\\ude00\", \"t\": true, \"n\": null,"
                      " \"o\": {\"k\": {\"deep\": 7}}}";
    json *j = json_parse(src, strlen(src));
    CHECK(j != NULL);
    CHECK(json_len(json_get(j, "a")) == 3);
    CHECK(json_number(json_at(json_get(j, "a"), 2), 0) == -300);
    CHECK_STR(json_string(json_get(j, "s"), ""), "x\n\xc3\xa9\xf0\x9f\x98\x80");
    CHECK(json_get(j, "t")->boolean);
    CHECK(json_get(j, "n")->type == JSON_NULL);
    CHECK(json_number(json_path(j, "o.k.deep"), 0) == 7);
    CHECK(json_path(j, "o.missing.deep") == NULL);
    json_free(j);

    const char *bad[] = {"{", "[1,]", "{\"a\" 1}", "tru", "\"unterminated", "[1] x", "{\"a\":\"\x01\"}"};
    for (size_t i = 0; i < ARRAY_LEN(bad); i++)
        CHECK(json_parse(bad[i], strlen(bad[i])) == NULL);

    char deep[400];
    memset(deep, '[', 200);
    memset(deep + 200, ']', 200);
    CHECK(json_parse(deep, sizeof deep) == NULL); /* deeper than 128 */
}

/* ------------------------------------------------------------------ http */

static void test_http(void) {
    const char *plain = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 4\r\n\r\n[42]trailing";
    int status;
    char *body;
    size_t n;
    CHECK(http_parse_response(plain, strlen(plain), &status, &body, &n) == 0);
    CHECK(status == 200 && n == 4);
    CHECK_STR(body, "[42]");
    free(body);

    const char *chunked =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\n[1,2\r\n2\r\n,3\r\n1\r\n]\r\n0\r\n\r\n";
    CHECK(http_parse_response(chunked, strlen(chunked), &status, &body, &n) == 0);
    CHECK_STR(body, "[1,2,3]");
    free(body);

    const char *eof = "HTTP/1.0 204 No Content\r\nServer: x\r\n\r\n";
    CHECK(http_parse_response(eof, strlen(eof), &status, &body, &n) == 0);
    CHECK(status == 204 && n == 0);
    free(body);

    const char *truncated = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc";
    CHECK(http_parse_response(truncated, strlen(truncated), &status, &body, &n) == -1);
    CHECK(http_parse_response("garbage", 7, &status, &body, &n) == -1);
}

/* ---------------------------------------------------------------- docker */

static void test_docker(void) {
    const char *list =
        "[{\"Id\":\"8163df7ade69aaaa\",\"Names\":[\"/exante-postgres-1\"],\"Image\":\"pgvector/pgvector:pg17\","
        "\"State\":\"running\",\"Status\":\"Up 3 days (healthy)\",\"Created\":1700000000,"
        "\"Labels\":{\"com.docker.compose.project\":\"exante\",\"com.docker.compose.service\":\"postgres\"},"
        "\"Ports\":[{\"IP\":\"127.0.0.1\",\"PrivatePort\":5432,\"PublicPort\":5433,\"Type\":\"tcp\"},"
        "{\"IP\":\"::\",\"PrivatePort\":5432,\"PublicPort\":5433,\"Type\":\"tcp\"},"
        "{\"PrivatePort\":9000,\"Type\":\"tcp\"}]},"
        "{\"Id\":\"ffff\",\"Names\":[\"/old\"],\"Image\":\"redis\",\"State\":\"exited\",\"Ports\":[]}]";
    docker_container *items;
    size_t n;
    CHECK(docker_parse_containers(list, strlen(list), &items, &n) == 0);
    CHECK(n == 2);
    CHECK_STR(items[0].name, "exante-postgres-1");
    CHECK_STR(items[0].project, "exante");
    CHECK(items[0].nports == 2); /* v4/v6 duplicate merged, unpublished kept */
    CHECK(items[0].ports[0].public_port == 5433 && items[0].ports[0].private_port == 5432);
    CHECK(items[0].ports[1].public_port == 0);
    CHECK_STR(items[1].state, "exited");
    free(items);

    const char *s1 = "{\"cpu_stats\":{\"cpu_usage\":{\"total_usage\":1000000000},\"system_cpu_usage\":100000000000,"
                     "\"online_cpus\":4},\"memory_stats\":{\"usage\":104857600,\"limit\":2147483648,"
                     "\"stats\":{\"inactive_file\":4857600}},\"pids_stats\":{\"current\":9}}";
    const char *s2 = "{\"cpu_stats\":{\"cpu_usage\":{\"total_usage\":1500000000},\"system_cpu_usage\":104000000000,"
                     "\"online_cpus\":4},\"memory_stats\":{\"usage\":104857600}}";
    docker_stats a, b;
    CHECK(docker_parse_stats(s1, strlen(s1), &a) == 0);
    CHECK(docker_parse_stats(s2, strlen(s2), &b) == 0);
    CHECK(a.mem_usage == 100000000);
    CHECK(a.pids == 9 && a.online_cpus == 4);
    double pct = docker_cpu_percent(&a, &b);
    CHECK(pct > 49.9 && pct < 50.1); /* 0.5s of CPU over 4s of system time on 4 cores */
    CHECK(docker_cpu_percent(&b, &a) < 0);

    CHECK_STR(docker_runtime_name("/Users/x/.orbstack/run/docker.sock"), "OrbStack");
    CHECK_STR(docker_runtime_name("/Users/x/.docker/run/docker.sock"), "Docker Desktop");
    CHECK_STR(docker_runtime_name("/var/run/docker.sock"), "docker");
}

/* ------------------------------------------------------------- procparse */

static void test_procparse(void) {
    const char *stat = "4123 (node (vite) x) S 4100 4100 4100 34816 4123 4194304 9000 0 12 0 250 50 0 0 20 0 11 0 "
                       "123456 1234567890 76800 18446744073709551615 1 1 0 0 0 0 0 16781312 17442 0 0 0 17 3 0 0 0 0 0";
    stat_fields f;
    CHECK(procparse_stat(stat, &f) == 0);
    CHECK_STR(f.name, "node (vite) x");
    CHECK(f.state == 'S' && f.ppid == 4100 && f.pgid == 4100);
    CHECK(f.utime == 250 && f.stime == 50 && f.threads == 11);
    CHECK(f.starttime == 123456 && f.rss_pages == 76800);

    net_entry e;
    const char *l4 = "   0: 0100007F:1F90 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 "
                     "55123 1 0000000000000000 100 0 0 10 0";
    CHECK(procparse_net_line(l4, false, &e) == 0);
    CHECK(e.port == 8080 && e.state == 0x0A && e.inode == 55123);
    char addr[64];
    procparse_addr(&e, addr, sizeof addr);
    CHECK_STR(addr, "127.0.0.1");
    const char *l6 = "   1: 00000000000000000000000000000000:1435 00000000000000000000000000000000:0000 0A "
                     "00000000:00000000 00:00000000 00000000   501        0 777 1 0000000000000000 100 0 0 10 0";
    CHECK(procparse_net_line(l6, true, &e) == 0);
    CHECK(e.port == 5173 && e.inode == 777);
    procparse_addr(&e, addr, sizeof addr);
    CHECK_STR(addr, "*");
    CHECK(procparse_net_line("  sl  local_address rem_address   st", false, &e) != 0);

    char cmd[64];
    procparse_cmdline("node\0server.js\0--port\0003000\0", 27, cmd, sizeof cmd);
    CHECK_STR(cmd, "node server.js --port 3000");
}

/* -------------------------------------------------------------- classify */

static void title_case(const char *name, const char *cmd, runtime want_rt, const char *want) {
    runtime rt = classify(name, cmd);
    char t[128];
    derive_title(rt, name, cmd, t, sizeof t);
    CHECK(rt == want_rt);
    CHECK_STR(t, want);
}

static void test_classify(void) {
    title_case("node", "node /p/node_modules/vite/bin/vite.js --port 5173", RT_NODE, "vite");
    title_case("node", "/usr/local/bin/node /usr/local/lib/node_modules/npm/bin/npm-cli.js run dev", RT_NODE,
               "npm run dev");
    title_case("node", "node server.js", RT_NODE, "server.js");
    title_case("node", "node --require ts-node/register src/main.ts", RT_NODE, "main.ts");
    title_case("node", "node /p/node_modules/.bin/tsx watch src/index.ts", RT_NODE, "tsx watch index.ts");
    title_case("node", "node /p/node_modules/@nestjs/cli/bin/nest.js start", RT_NODE, "@nestjs/cli start");
    title_case("next-server (v1", "next-server (v15.1.0)", RT_NODE, "next-server");
    title_case("bun", "bun run dev", RT_BUN, "run dev");
    title_case("deno", "deno task start", RT_DENO, "task start");
    title_case("Python", "/opt/py/bin/python3 -m uvicorn app.main:app --reload", RT_PYTHON, "uvicorn app.main:app");
    title_case("python3.12", "python3 manage.py runserver 0.0.0.0:8000", RT_PYTHON, "manage.py runserver 0.0.0.0:8000");
    title_case("uvicorn", "/venv/bin/python /venv/bin/uvicorn main:app", RT_PYTHON, "uvicorn main:app");
    title_case("python3.12", "/venv/bin/python3 -c from multiprocessing.spawn import spawn_main; spawn_main()",
               RT_PYTHON, "python3 mp-worker");
    title_case("esbuild", "/p/node_modules/@esbuild/darwin-arm64/bin/esbuild --service=0.25.3 --ping", RT_NODE,
               "esbuild");
    title_case("java", "java -Xmx2g -cp a.jar:b.jar org.gradle.launcher.daemon.bootstrap.GradleDaemon 8.5", RT_JAVA,
               "GradleDaemon");
    title_case("java", "java -jar /srv/app.jar", RT_JAVA, "app.jar");
    title_case("go", "go run ./cmd/api", RT_GO, "go run api");
    title_case("main", "/tmp/go-build123/b001/exe/main", RT_GO, "main");
    title_case("com.docker.backend", "/Applications/Docker.app/Contents/MacOS/com.docker.backend", RT_PROXY,
               "com.docker.backend");
    title_case("postgres", "postgres -D /usr/local/var/postgres", RT_NONE, "postgres");
    title_case("phpstorm", "/Applications/PhpStorm.app/Contents/MacOS/phpstorm", RT_NONE, "phpstorm");
    CHECK_STR(runtime_badge(RT_PYTHON), "PY");
}

/* ----------------------------------------------------------------- keys */

static void test_keys(void) {
    key_event ev;
    const unsigned char up[] = "\x1b[A";
    CHECK(key_parse(up, 3, &ev, false) == 3 && ev.type == KEY_UP);
    const unsigned char pgdn[] = "\x1b[6~";
    CHECK(key_parse(pgdn, 4, &ev, false) == 4 && ev.type == KEY_PGDN);
    const unsigned char partial[] = "\x1b[";
    CHECK(key_parse(partial, 2, &ev, false) == 0);
    CHECK(key_parse(partial, 2, &ev, true) == 1 && ev.type == KEY_ESC);
    const unsigned char esc[] = "\x1b";
    CHECK(key_parse(esc, 1, &ev, true) == 1 && ev.type == KEY_ESC);
    const unsigned char wheel[] = "\x1b[<65;10;5M";
    CHECK(key_parse(wheel, 11, &ev, false) == 11 && ev.type == KEY_WHEEL_DOWN);
    const unsigned char click[] = "\x1b[<0;12;7M";
    CHECK(key_parse(click, 10, &ev, false) == 10 && ev.type == KEY_CLICK && ev.x == 11 && ev.y == 6);
    const unsigned char ctrl[] = "\x03";
    CHECK(key_parse(ctrl, 1, &ev, false) == 1 && ev.type == KEY_CTRL && ev.ch == 'c');
    const unsigned char zh[] = "\xd0\xb6";
    CHECK(key_parse(zh, 1, &ev, false) == 0);
    CHECK(key_parse(zh, 2, &ev, false) == 2 && ev.type == KEY_CHAR && ev.ch == 0x436);
    const unsigned char btab[] = "\x1b[Z";
    CHECK(key_parse(btab, 3, &ev, false) == 3 && ev.type == KEY_BTAB);
}

/* ------------------------------------------------------- app + kill tree */

static entry mk(const char *key, entry_kind kind, runtime rt, const char *title, double cpu, uint16_t port) {
    entry e;
    memset(&e, 0, sizeof e);
    str_copy(e.key, sizeof e.key, key);
    e.kind = kind;
    e.rt = rt;
    e.running = true;
    str_copy(e.title, sizeof e.title, title);
    str_copy(e.name, sizeof e.name, title);
    e.has_cpu = true;
    e.cpu = cpu;
    if (port) {
        e.ports[0].port = port;
        e.nports = 1;
    }
    return e;
}

static key_event ch(uint32_t c) {
    key_event ev = {.type = KEY_CHAR, .ch = c};
    return ev;
}

static void test_app(void) {
    entry items[] = {
        mk("a", ENT_PROC, RT_NODE, "vite", 12, 5173),
        mk("b", ENT_PROC, RT_NONE, "launchd", 1, 0),
        mk("c", ENT_PROC, RT_NONE, "ControlCenter", 0.5, 7000),
        mk("d", ENT_CONTAINER, RT_DOCKER, "postgres-1", 3, 5433),
        mk("e", ENT_PROC, RT_PROXY, "com.docker.backend", 2, 0),
        mk("f", ENT_PROC, RT_PYTHON, "uvicorn main:app", 40, 8000),
    };
    model m;
    model_init(&m);
    m.items = items;
    m.n = ARRAY_LEN(items);

    app a;
    app_init(&a, MODE_DEV);
    app_rebuild(&a, &m);
    CHECK(a.nview == 4); /* vite, ControlCenter (port), postgres, uvicorn; not launchd, not idle proxy */
    CHECK(a.counts[MODE_PORTS] == 4);
    CHECK(a.counts[MODE_ALL] == 6);
    CHECK_STR(m.items[a.view[0]].title, "uvicorn main:app"); /* CPU desc */

    /* Selection follows the entry when the order changes. */
    key_event down = {.type = KEY_DOWN};
    app_key(&a, &m, &down);
    CHECK_STR(app_selected(&a, &m)->title, "vite");
    items[0].cpu = 99;
    app_rebuild(&a, &m);
    CHECK(a.sel == 0);
    CHECK_STR(app_selected(&a, &m)->title, "vite");

    /* Filter by port. */
    key_event k = ch('/');
    app_key(&a, &m, &k);
    CHECK(a.editing);
    const char *typed = ":54";
    for (const char *p = typed; *p; p++) {
        k = ch((uint32_t)*p);
        app_key(&a, &m, &k);
    }
    CHECK(a.nview == 1);
    CHECK_STR(app_selected(&a, &m)->title, "postgres-1");
    k.type = KEY_ESC;
    app_key(&a, &m, &k);
    CHECK(!a.editing && a.filter[0] == '\0' && a.nview == 4);

    /* Mode switch and sort. */
    k.type = KEY_TAB;
    app_key(&a, &m, &k);
    CHECK(a.mode == MODE_PORTS);
    k = ch('s');
    app_key(&a, &m, &k);
    CHECK(a.sort == SORT_MEM);
    k = ch('s');
    app_key(&a, &m, &k);
    CHECK(a.sort == SORT_PORT);
    CHECK(m.items[a.view[0]].ports[0].port == 5173);

    /* x on a container asks first, y confirms, and the action carries the container id. */
    a.sel_key[0] = '\0';
    for (size_t i = 0; i < a.nview; i++)
        if (m.items[a.view[i]].kind == ENT_CONTAINER) a.sel = (int)i;
    str_copy(items[3].id, sizeof items[3].id, "8163df7ade69");
    k = ch('x');
    action act = app_key(&a, &m, &k);
    CHECK(act.type == ACT_NONE && a.ovl == OVL_CONFIRM);
    k = ch('y');
    act = app_key(&a, &m, &k);
    CHECK(act.type == ACT_DOCKER_STOP);
    CHECK_STR(act.id, "8163df7ade69");

    /* Anything but y aborts. */
    k = ch('X');
    app_key(&a, &m, &k);
    k = ch('n');
    act = app_key(&a, &m, &k);
    CHECK(act.type == ACT_NONE && a.ovl == OVL_NONE);

    k = ch('q');
    CHECK(app_key(&a, &m, &k).type == ACT_QUIT);
    app_free(&a);
}

static void test_kill_tree(void) {
    sys_proc procs[6];
    memset(procs, 0, sizeof procs);
    int pids[] = {1, 100, 200, 201, 300, 301}, ppids[] = {0, 1, 100, 100, 200, 1};
    for (int i = 0; i < 6; i++) {
        procs[i].pid = pids[i];
        procs[i].ppid = ppids[i];
        procs[i].start = 1000 + i;
    }
    pid_ref out[KILL_MAX];
    int n = kill_collect(procs, 6, 100, out, KILL_MAX);
    CHECK(n == 4);
    CHECK(out[0].pid == 100 && out[0].start == 1001);
    bool has300 = false, has301 = false;
    for (int i = 0; i < n; i++) {
        has300 |= out[i].pid == 300;
        has301 |= out[i].pid == 301;
    }
    CHECK(has300 && !has301);
    CHECK(kill_collect(procs, 6, 999, out, KILL_MAX) == 0);

    kill_job jobs[KILL_JOBS];
    memset(jobs, 0, sizeof jobs);
    char msg[160];
    CHECK(kill_start(jobs, procs, 6, 1, 1000, SIGTERM, "launchd", msg, sizeof msg) == LVL_ERR);
}

/* ---------------------------------------------------------------- screen */

static void test_screen(void) {
    scr_init(COLOR_TRUE);
    char buf[96];
    scr_sgr(buf, 0x00f0ff, 0x0a0514, ATTR_BOLD);
    CHECK_STR(buf, "\x1b[0;1;38;2;0;240;255;48;2;10;5;20m");
    scr_init(COLOR_256);
    scr_sgr(buf, 0xff0000, RGB_DEFAULT, 0);
    CHECK_STR(buf, "\x1b[0;38;5;196;49m");
    scr_init(COLOR_NONE);
    scr_sgr(buf, 0xff0000, 0x00ff00, ATTR_REVERSE);
    CHECK_STR(buf, "\x1b[0;7m");
    CHECK(rgb_mix(0x000000, 0xffffff, 0.5f) == 0x7f7f7f);
}

int main(void) {
    struct {
        const char *name;
        void (*fn)(void);
    } suites[] = {
        {"util", test_util},           {"json", test_json},         {"http", test_http}, {"docker", test_docker},
        {"procparse", test_procparse}, {"classify", test_classify}, {"keys", test_keys}, {"app", test_app},
        {"kill", test_kill_tree},      {"screen", test_screen},
    };
    for (size_t i = 0; i < ARRAY_LEN(suites); i++) {
        int before = failures;
        suites[i].fn();
        printf("%-10s %s\n", suites[i].name, failures == before ? "ok" : "FAILED");
    }
    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
