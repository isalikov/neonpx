# Changelog

## 1.0.0 — 2026-10-10

- First version: a live TUI of dev processes and Docker containers with their TCP ports, CPU, and memory.
- Three modes: DEV (runtimes, port holders, containers), PORTS, and ALL; filter (including `:port`); six sort keys.
- Process-tree kill with confirmation: SIGTERM with automatic SIGKILL escalation after 3 s, or SIGKILL directly; pid-reuse and self-kill guards.
- Docker Engine API over the unix socket: container stats, stop, kill, restart, and start. Port forwarders are attributed to their containers.
- macOS backend (libproc, sysctl, Mach) and Linux backend (`/proc`, `/proc/net/tcp{,6}`).
- `neon`, `synthwave`, and `matrix` themes; truecolor / 256 / 16 / no-color output; `--no-bg`.
- `--list` for a plain-text snapshot.
- Makefile tooling (sanitizers, `check`, `fmt`, `dist`, `linux`); CI; release workflow for macOS universal and static Linux x86_64/aarch64 binaries; install scripts.
