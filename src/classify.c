#include "classify.h"

#include <stdio.h>
#include <string.h>

#include "util.h"

#define MAX_TOKENS 16
#define TOKEN_LEN  192

typedef struct {
    char t[MAX_TOKENS][TOKEN_LEN];
    int n;
} tokens;

static void tokenize(const char *cmd, tokens *tk) {
    tk->n = 0;
    while (*cmd && tk->n < MAX_TOKENS) {
        while (*cmd == ' ')
            cmd++;
        if (!*cmd) break;
        const char *end = strchr(cmd, ' ');
        size_t l = end ? (size_t)(end - cmd) : strlen(cmd);
        str_ncopy(tk->t[tk->n++], TOKEN_LEN, cmd, l);
        cmd += l;
    }
}

const char *runtime_badge(runtime rt) {
    static const char *const badges[RT_COUNT] = {
        [RT_NONE] = "PROC", [RT_NODE] = "NODE",   [RT_BUN] = "BUN",    [RT_DENO] = "DENO",    [RT_PYTHON] = "PY",
        [RT_RUBY] = "RUBY", [RT_JAVA] = "JVM",    [RT_GO] = "GO",      [RT_PHP] = "PHP",      [RT_DOTNET] = ".NET",
        [RT_BEAM] = "BEAM", [RT_DOCKER] = "DOCK", [RT_PROXY] = "PRXY", [RT_UNKNOWN] = "????",
    };
    return rt < RT_COUNT ? badges[rt] : "PROC";
}

static bool is_proxy(const char *name, const char *a0) {
    static const char *const names[] = {
        "com.docker.backend", "com.docker.vpnkit", "vpnkit-bridge", "docker-proxy",
        "OrbStack Helper",    "orbstack",          "limactl",       "gvproxy",
        "rootlessport",       "rootlesskit",       "slirp4netns",   "pasta",
        "com.docker.vmnetd",
    };
    for (size_t i = 0; i < ARRAY_LEN(names); i++)
        if (str_iequal(name, names[i]) || str_iequal(a0, names[i])) return true;
    return str_istarts(name, "com.docker.");
}

runtime classify(const char *name, const char *cmd) {
    tokens tk;
    tokenize(cmd, &tk);
    const char *a0 = tk.n ? path_base(tk.t[0]) : "";

    if (is_proxy(name, a0)) return RT_PROXY;

    static const struct {
        const char *word;
        runtime rt;
        bool prefix;
    } rules[] = {
        {"node", RT_NODE, false},        {"nodejs", RT_NODE, false},
        {"next-server", RT_NODE, true},  {"bun", RT_BUN, false},
        {"deno", RT_DENO, false},        {"python", RT_PYTHON, true},
        {"uvicorn", RT_PYTHON, false},   {"gunicorn", RT_PYTHON, false},
        {"hypercorn", RT_PYTHON, false}, {"celery", RT_PYTHON, false},
        {"daphne", RT_PYTHON, false},    {"flask", RT_PYTHON, false},
        {"jupyter", RT_PYTHON, true},    {"streamlit", RT_PYTHON, false},
        {"ruby", RT_RUBY, false},        {"puma", RT_RUBY, true},
        {"rails", RT_RUBY, false},       {"sidekiq", RT_RUBY, false},
        {"java", RT_JAVA, false},        {"go", RT_GO, false},
        {"php", RT_PHP, false},          {"php-fpm", RT_PHP, true},
        {"php8", RT_PHP, true},          {"dotnet", RT_DOTNET, false},
        {"beam.smp", RT_BEAM, false},
    };
    for (size_t i = 0; i < ARRAY_LEN(rules); i++) {
        bool hit = rules[i].prefix ? (str_istarts(name, rules[i].word) || str_istarts(a0, rules[i].word))
                                   : (str_iequal(name, rules[i].word) || str_iequal(a0, rules[i].word));
        if (hit) return rules[i].rt;
    }
    if (strstr(cmd, "/go-build")) return RT_GO;
    if (tk.n && strstr(tk.t[0], "node_modules/")) return RT_NODE;
    return RT_NONE;
}

/* "…/node_modules/vite/bin/vite.js" → "vite", "…/node_modules/.bin/tsx" → "tsx", "@nestjs/cli". */
static void pretty_script(const char *path, char *out, size_t cap) {
    const char *nm = NULL, *p = path;
    while ((p = strstr(p, "node_modules/"))) {
        nm = p + 13;
        p = nm;
    }
    if (nm) {
        if (strncmp(nm, ".bin/", 5) == 0) {
            str_copy(out, cap, path_base(nm + 5));
            return;
        }
        const char *slash = strchr(nm, '/');
        if (nm[0] == '@' && slash) slash = strchr(slash + 1, '/');
        str_ncopy(out, cap, nm, slash ? (size_t)(slash - nm) : strlen(nm));
        return;
    }
    str_copy(out, cap, path_base(path));
}

static bool is_flag(const char *t) { return t[0] == '-'; }

/* Appends up to `max` arguments directly following tokens[i], stopping at the first flag. */
static void append_args(const tokens *tk, int i, int max, char *out, size_t cap) {
    size_t len = strlen(out);
    for (int k = 0; k < max && i < tk->n && !is_flag(tk->t[i]); k++, i++) {
        const char *a = strchr(tk->t[i], '/') ? path_base(tk->t[i]) : tk->t[i];
        str_append(out, cap, &len, " ", 1);
        str_append(out, cap, &len, a, strlen(a));
    }
}

static void title_js(const tokens *tk, const char *name, char *out, size_t cap) {
    const char *a0 = path_base(tk->t[0]);
    if (!str_iequal(a0, "node") && !str_iequal(a0, "nodejs") && !str_iequal(a0, "bun") && !str_iequal(a0, "deno")) {
        /* process.title was rewritten ("next-server (v15.1.0)", "npm run dev"). */
        char buf[192];
        size_t len = 0;
        buf[0] = '\0';
        for (int i = 0; i < tk->n && i < 4; i++) {
            if (tk->t[i][0] == '(' || (i > 0 && is_flag(tk->t[i]))) break;
            const char *a = i == 0 ? a0 : tk->t[i];
            if (len) str_append(buf, sizeof buf, &len, " ", 1);
            str_append(buf, sizeof buf, &len, a, strlen(a));
        }
        str_copy(out, cap, buf[0] ? buf : name);
        return;
    }
    int i = 1;
    char sub[32] = "";
    while (i < tk->n) {
        const char *t = tk->t[i];
        if (!is_flag(t)) break;
        i++;
        if (strcmp(t, "-r") == 0 || strcmp(t, "--require") == 0 || strcmp(t, "--import") == 0 ||
            strcmp(t, "--loader") == 0 || strcmp(t, "--experimental-loader") == 0)
            i++;
    }
    if (i < tk->n && !str_iequal(a0, "node") && !str_iequal(a0, "nodejs")) {
        const char *t = tk->t[i];
        if (strcmp(t, "run") == 0 || strcmp(t, "x") == 0 || strcmp(t, "task") == 0 || strcmp(t, "test") == 0 ||
            strcmp(t, "serve") == 0) {
            str_copy(sub, sizeof sub, t);
            i++;
            while (i < tk->n && is_flag(tk->t[i]))
                i++;
        }
    }
    if (i >= tk->n) {
        str_copy(out, cap, sub[0] ? sub : name);
        return;
    }
    char script[128];
    pretty_script(tk->t[i], script, sizeof script);
    const char *ext[] = {".js", ".cjs", ".mjs"};
    if (strstr(tk->t[i], "node_modules/"))
        for (size_t e = 0; e < ARRAY_LEN(ext); e++)
            if (str_ends(script, ext[e])) script[strlen(script) - strlen(ext[e])] = '\0';
    if (strcmp(script, "npm-cli") == 0) str_copy(script, sizeof script, "npm");
    if (strcmp(script, "npx-cli") == 0) str_copy(script, sizeof script, "npx");
    size_t len = 0;
    out[0] = '\0';
    if (sub[0]) {
        str_append(out, cap, &len, sub, strlen(sub));
        str_append(out, cap, &len, " ", 1);
    }
    str_append(out, cap, &len, script, strlen(script));
    append_args(tk, i + 1, 2, out, cap);
}

static void title_script(const tokens *tk, const char *name, char *out, size_t cap) {
    int i = 1;
    while (i < tk->n && is_flag(tk->t[i])) {
        if (strcmp(tk->t[i], "-c") == 0) {
            /* multiprocessing / reloader children run inline code */
            bool mp =
                i + 2 < tk->n && strcmp(tk->t[i + 1], "from") == 0 && str_istarts(tk->t[i + 2], "multiprocessing");
            snprintf(out, cap, "%s %s", path_base(tk->t[0]), mp ? "mp-worker" : "-c");
            return;
        }
        if (strcmp(tk->t[i], "-m") == 0 && i + 1 < tk->n) {
            str_copy(out, cap, tk->t[i + 1]);
            append_args(tk, i + 2, 2, out, cap);
            return;
        }
        i++;
    }
    if (i >= tk->n) {
        str_copy(out, cap, name);
        return;
    }
    str_copy(out, cap, path_base(tk->t[i]));
    append_args(tk, i + 1, 2, out, cap);
}

static void title_java(const tokens *tk, const char *name, char *out, size_t cap) {
    for (int i = 1; i < tk->n; i++) {
        const char *t = tk->t[i];
        if (strcmp(t, "-jar") == 0 && i + 1 < tk->n) {
            str_copy(out, cap, path_base(tk->t[i + 1]));
            return;
        }
        if (strcmp(t, "-cp") == 0 || strcmp(t, "-classpath") == 0 || strcmp(t, "--class-path") == 0 ||
            strcmp(t, "-p") == 0 || strcmp(t, "--module-path") == 0) {
            i++;
            continue;
        }
        if ((strcmp(t, "-m") == 0 || strcmp(t, "--module") == 0) && i + 1 < tk->n) {
            str_copy(out, cap, tk->t[i + 1]);
            return;
        }
        if (is_flag(t)) continue;
        const char *dot = strrchr(t, '.');
        str_copy(out, cap, dot && dot[1] ? dot + 1 : t);
        return;
    }
    str_copy(out, cap, name);
}

void derive_title(runtime rt, const char *name, const char *cmd, char *out, size_t cap) {
    tokens tk;
    tokenize(cmd, &tk);
    if (!tk.n) {
        str_copy(out, cap, name);
        return;
    }
    switch (rt) {
    case RT_NODE:
    case RT_BUN:
    case RT_DENO: title_js(&tk, name, out, cap); break;
    case RT_PYTHON:
    case RT_RUBY:
    case RT_PHP: title_script(&tk, name, out, cap); break;
    case RT_JAVA: title_java(&tk, name, out, cap); break;
    case RT_GO:
    case RT_DOTNET:
        str_copy(out, cap, name);
        if (str_iequal(path_base(tk.t[0]), "go") || str_iequal(path_base(tk.t[0]), "dotnet"))
            append_args(&tk, 1, 2, out, cap);
        break;
    default: str_copy(out, cap, name); break;
    }
}
