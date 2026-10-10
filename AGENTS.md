# AGENTS.md

Guidance for AI coding agents working in this repository.

## Working rules

1. **Never change git state** beyond read-only commands (`status`, `diff`, `log`, `show`, `blame`). The user commits by hand.
2. **End every session that changed files by suggesting one commit message** for everything uncommitted (`feat/`, `fix/`, or `chore/` prefix, imperative subject, short body). Base it on `git status --short` and `git diff`, not on memory.
3. **Keep README.md and CHANGELOG.md in sync** when keys, modes, options, environment variables, or the release layout change.
4. `make check` must pass: it builds with `-Werror`, runs the tests under ASan/UBSan, and runs clang-format, clang-tidy, and cppcheck when they are installed.

## What this is

neonpx is a terminal UI that lists dev processes (node, python, bun, …) and Docker containers with their TCP ports, CPU, and memory, and can kill them. It is plain C11 plus POSIX: no third-party code, no ncurses, and no subprocesses (it never runs `lsof`, `ps`, or `docker`).

## Commands

```sh
make build | run | test | check | fmt | release | dist | linux | clean
make test SAN=0            # without sanitizers (musl has no ASan)
make linux                 # static Linux build in alpine via Docker, for testing from macOS
./build/debug/neonpx --list --all   # non-interactive snapshot, handy for checking the backends
```

The version comes from `git describe` into the generated `build/version.h`. Override it with `VERSION=…`.

## Architecture

One main thread runs the UI. A background thread polls Docker.

- `sys.h` is the platform layer: `sys_darwin.c` (sysctl `KERN_PROC_ALL`, libproc task/fd/socket info, Mach host stats) and `sys_linux.c` (`/proc/*/stat`, `/proc/net/tcp{,6}`, then inodes matched to `/proc/*/fd`). Each file compiles to nothing on the other OS. `procparse.c` holds the Linux text parsers in portable form so they are unit-tested on macOS too.
- `model.c` is called once per interval by `model_refresh(m, docker_snapshot)`. It merges processes, listeners, and containers into `entry` rows sorted by a stable `key` (`p<pid>:<start>`, `d<container id>`, `o<port>`). CPU% comes from deltas against the previous sample, keyed by pid and start time. argv/cwd are cached per (pid, start, name). Ports that a running container publishes are removed from `RT_PROXY` processes (Docker Desktop, OrbStack, lima, docker-proxy, …), so the container row owns them.
- `classify.c` turns name + argv into a `runtime` badge and a human title. Add cases to `tests/test_main.c` (`title_case`) when you change it.
- `docker.c` contains a synchronous `docker_client` (socket discovery, HTTP/1.0 over a unix socket via `http.c`, JSON via `json.c`) and the poller thread (`docker_start`, `docker_snapshot_get`, `docker_submit`, `docker_poll_message`). Container CPU uses `one-shot=true` stats and the previous sample kept by the client.
- `app.c` is a pure state machine: modes, filter, sort, selection, overlays. `app_key()` returns an `action`. Selection follows `sel_key` across re-sorts. A kill target is captured into `pending` when the dialog opens. `app.c` performs no I/O.
- `kill.c` collects the descendants of the target, signals them, and tracks jobs: it reports deaths and escalates SIGTERM to SIGKILL after `KILL_GRACE_MS`. Every signal re-checks `sys_pid_start()` to avoid pid reuse. Trees that contain neonpx itself, and pid ≤ 1, are refused.
- `term.c` handles raw mode, the alt screen, SGR mouse, and `key_parse` (pure and tested). `screen.c` keeps a double-buffered cell grid and emits diffed output in synchronized-update brackets, with truecolor, 256, 16, or no color. `ui.c` only renders; `theme.c` holds the palettes.

## Conventions

- Allocation goes through `xmalloc`/`xcalloc`/`xrealloc`, which abort on OOM. Strings go through `str_copy`/`str_append`, which never split UTF-8.
- Every glyph drawn must be single-width. `cp_width()` classifies codepoints, and wide ones become `·`. Avoid emoji-presentation characters (⏳, ⚡) in messages.
- Status messages start with a narrow glyph: `✔` ok, `✖` error, `↯` signal sent, `☠` escalation, `»` in progress.
- Keep platform `#ifdef`s inside `sys_*.c` (and `http.c` for `MSG_NOSIGNAL` / `SO_NOSIGPIPE`).
