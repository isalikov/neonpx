#ifndef NEONPX_CLASSIFY_H
#define NEONPX_CLASSIFY_H

#include <stddef.h>

typedef enum {
    RT_NONE,
    RT_NODE,
    RT_BUN,
    RT_DENO,
    RT_PYTHON,
    RT_RUBY,
    RT_JAVA,
    RT_GO,
    RT_PHP,
    RT_DOTNET,
    RT_BEAM,
    RT_DOCKER,  /* a container */
    RT_PROXY,   /* Docker / VM port-forwarding infrastructure */
    RT_UNKNOWN, /* a port whose owner we cannot see */
    RT_COUNT
} runtime;

/* Five-column badge text, e.g. "NODE", "PY". */
const char *runtime_badge(runtime rt);
runtime classify(const char *name, const char *cmd);
/* Human title: "vite", "npm run dev", "uvicorn app.main:app", "next-server". */
void derive_title(runtime rt, const char *name, const char *cmd, char *out, size_t cap);

#endif
