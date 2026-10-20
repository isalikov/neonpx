# NeonPX

<p align="center">
  <strong>Who holds my ports, and how hard are they burning.</strong><br>
  A cyberpunk terminal monitor for dev processes and Docker containers on macOS and Linux.
</p>

<p align="center">
  <a href="https://github.com/isalikov/neonpx/releases"><img src="https://img.shields.io/github/v/release/isalikov/neonpx?style=flat-square&color=ff2a6d" alt="Latest release"></a>
  <a href="https://github.com/isalikov/neonpx/actions/workflows/ci.yml"><img src="https://img.shields.io/github/actions/workflow/status/isalikov/neonpx/ci.yml?style=flat-square&label=ci&color=00f0ff" alt="CI"></a>
</p>

NeonPX shows the `node` / `python` / `bun` / `deno` / … processes and Docker
containers on your machine, which TCP ports they hold, and how much CPU and
memory they use. Any of them can be killed from the same screen: process
trees get SIGTERM, then SIGKILL if they hold on, and containers get
`docker stop` / `kill` / `restart`.

It is a single C11 binary with no runtime dependencies. On macOS it uses
libproc, on Linux `/proc`, and it talks to the Docker Engine API over the
unix socket directly. It has no ncurses, no `lsof`, and doesn't shell out
to the `docker` CLI.

## Install

```sh
curl -fsSL https://dwnl.cc/install-neonpx | sh
```

The script downloads the latest release for your platform. macOS gets one
universal arm64 + x86_64 binary. Linux gets a fully static musl binary for
x86_64 or aarch64. The script verifies the SHA-256 and installs to
`~/.local/bin/neonpx` (`NEONPX_PREFIX` overrides the prefix). If no release
asset exists, it builds from source with `make` and `cc`.

Uninstall:

```sh
curl -fsSL https://dwnl.cc/uninstall-neonpx | sh
```

## Usage

```sh
neonpx              # DEV mode: runtimes, port holders, containers
neonpx --ports      # everything listening on TCP
neonpx --all        # every visible process
neonpx --list       # one plain-text snapshot, for scripts and pipes
sudo neonpx         # see and kill processes owned by other users
```

| Option | |
| --- | --- |
| `-a`, `--all` / `-p`, `--ports` | start in ALL / PORTS mode |
| `-i`, `--interval SEC` | sampling interval, 0.25–10 s (default 1) |
| `-t`, `--theme NAME` | `neon` (default), `synthwave`, `matrix` |
| `--no-bg` | keep the terminal's own background (for translucent terminals) |
| `--no-docker` | never touch the Docker socket |
| `-l`, `--list` | print one snapshot as a table and exit |

Environment: `NEONPX_THEME`, `NEONPX_COLOR` (`truecolor`, `256`, `16`, `none`),
`NO_COLOR`, `DOCKER_HOST` (`unix://` only). Truecolor is detected from
`COLORTERM` and well-known terminals; otherwise NeonPX uses the 256-color palette.

## Keyboard

| Key | Action |
| --- | --- |
| `j` `k` / `↑` `↓` | move (mouse wheel and click work too) |
| `g` `G` / `PgUp` `PgDn` / `ctrl-d` `ctrl-u` | top, bottom, page |
| `tab` / `1` `2` `3` | mode: DEV → PORTS → ALL |
| `s` / `S` | cycle the sort column (CPU, MEM, PORT, NAME, PID, UPTIME) / reverse it |
| `/` | filter by name, command, project, image, pid; `:5173` matches a port |
| `x` | process: SIGTERM the tree, SIGKILL after 3 s if it survives · container: `docker stop` |
| `X` | process: SIGKILL the tree · container: `docker kill` |
| `r` | container: `docker restart`, or `docker start` if it is stopped |
| `R` / `ctrl-l` | refresh now |
| `?` | help |
| `esc` | clear the filter / close a dialog |
| `q` / `ctrl-c` | quit |

Every kill asks for confirmation (`y` / `enter` confirms, anything else aborts).

## What you see

- **KIND**: the runtime badge (`NODE`, `PY`, `BUN`, `DENO`, `RUBY`, `JVM`,
  `GO`, `PHP`, `.NET`, `BEAM`, `DOCK`, `PROC`). `PRXY` marks Docker / VM
  port forwarders, and `????` marks a port whose owner this user cannot see.
- **NAME**: a readable title from the command line, e.g. `vite`,
  `npm run dev`, `uvicorn app.main:app`, `next-server`, `manage.py runserver`.
- **PROJECT**: the process's working directory, or the compose project for
  containers.
- **PORTS**: listening TCP ports. For containers this is `host→container`.
- **CPU% / LOAD / MEM**: CPU is a percentage of one core, like `top`; LOAD is
  a sparkline of recent samples; MEM is resident memory (for containers,
  usage minus page cache, like `docker stats`).
- The **TARGET** panel at the bottom shows the selected row's full command,
  cwd, parent, children, bind addresses, or image, status, and compose service.

### Docker

NeonPX finds the daemon at `$DOCKER_HOST` or the first answering socket
among Docker Desktop, OrbStack, colima, Rancher Desktop, rootless Docker,
`/var/run/docker.sock`, and podman.

On macOS a published port is actually held by the VM's port forwarder
(`com.docker.backend`, OrbStack, `limactl`, …). NeonPX attributes those ports
to the container rows and hides the forwarder, so killing ":5432" stops the
container instead of Docker itself. If you do select a forwarder, the
confirmation warns you.

### Safety

- The kill target is captured when you press `x`, so re-sorting while the
  dialog is open can't retarget it. Before signalling, NeonPX checks that the
  pid still has the same start time, which guards against pid reuse.
- Children are collected from the process tree, so `npm run dev` goes down
  together with its `node`.
- NeonPX refuses pid 1 and any tree that contains NeonPX itself (for
  example, the shell that launched it).

## Build

Requirements: a C11 compiler (clang or gcc) and GNU make. Nothing else.

```sh
make build     # debug build with ASan/UBSan -> build/debug/neonpx
make run       # build and run (ARGS='--all')
make test      # unit tests under sanitizers
make check     # -Werror build + tests + clang-format/clang-tidy/cppcheck when installed
make release   # optimized, stripped -> build/release/neonpx
make release UNIVERSAL=1   # macOS arm64 + x86_64 in one binary
make release STATIC=1      # fully static (use with musl)
make linux     # static Linux binary via Docker (alpine), handy on a Mac
make install   # -> ~/.local/bin (PREFIX=...)
make dist      # release tarball in dist/
```

The default version is `1.0.1`, set in `Makefile`; override it with `VERSION=…`.

Releases are built by `.github/workflows/release.yml` when a `v*` tag is
pushed. It produces `neonpx-macos-universal.tar.gz`,
`neonpx-linux-x86_64.tar.gz`, `neonpx-linux-aarch64.tar.gz`, and
`sha256sums.txt`.

## Limits

- Only TCP listeners are tracked; UDP is not.
- Without root, other users' processes are visible but can't be signalled.
  On macOS their sockets can't be inspected either; on Linux such ports show
  up as `????` rows.
- Container CPU is measured inside the Docker VM on macOS, so it is relative
  to the VM's cores.
