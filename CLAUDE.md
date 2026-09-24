# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**MachineMonitor** is a real-time system metrics dashboard. It streams CPU, memory, network, and Docker container statistics to a retro-styled web interface with live charts and fast updates (250ms). Runs locally or in Docker; supports macOS hardware temperature sensors.

## Quick Start

**Local development (Linux):**

```bash
gcc -O2 -Wall -o machinemonitor app.c -lpthread
./machinemonitor
```

Visit `http://localhost:8000` (or set `ENV_HOST_PROC` to point at an alternate procfs).

**Docker:**

```bash
docker-compose up
```

Runs on `http://localhost:7777` with access to host /proc, /sys, and Docker daemon.

## Architecture

### Backend (`app.c`)

- **Language/runtime**: Plain C (C11), no external libraries beyond libc and pthreads
- **HTTP server**: Hand-rolled — a blocking `accept()` loop hands each connection to a detached pthread; requests are parsed manually (method + path only, headers otherwise ignored)
- **Metric collection**: Reads `/proc/stat`, `/proc/meminfo`, and `/proc/net/dev` directly (prefixed with `$ENV_HOST_PROC` when set) instead of using psutil
- **No macOS temperature support**: the container only ever runs on Linux, so the IOKit HID temperature code from the old Python version was dropped rather than ported (it was dead code there too — never wired into the metrics payload)
- **Endpoints**:
  - `/stream` — Server-Sent Events endpoint, yields metrics every 250ms (4x per second); each connection runs its own loop on its own thread
  - `/metrics` — Single JSON response of current metrics
  - `/` and other paths — serves files from `./static` (path-traversal guarded, `/` maps to `static/index.html`)
- **Docker Integration**: Talks directly to the Docker daemon's HTTP API over its unix socket (`$ENV_DOCKER_SOCK`, default `/var/run/docker.sock`) — no `docker` CLI binary in the image. Lists containers via `GET /containers/json`, then fetches `GET /containers/<id>/stats?stream=false` per container; parses the JSON with lightweight, order-dependent key lookup (not a full JSON parser — relies on the Docker API's documented, stable field order), sorts by CPU usage (top 25)
- **JSON output**: Hand-built via a growable string buffer (`sbuf_t`) with a small escaper for string values (container names, mem usage strings, hostname)

### Data Structure

Metrics are JSON objects with:

```javascript
{
  hostname: string,
  ts: milliseconds,
  boot_time: milliseconds,
  cpu: { percent, cores: [], count },
  memory: { percent, used, total, available },
  swap: { percent, used, total },
  network: { sent_rate, recv_rate, sent_total, recv_total },
  docker: [
    { name: string, cpu_percent: float, mem_usage: string, mem_percent: float }
  ]
}
```

**Key implementation**: Network rates are calculated by tracking previous counter values and computing bytes/second. Global `_prev_net` and `_prev_net_time` track state between requests.

### Frontend (`static/index.html`)

- Single HTML file with inline CSS and vanilla JavaScript
- **Design**: Retro CRT aesthetic (bright green on dark), scanlines, vignette
- **Updates**: Connects via EventSource to `/stream` for real-time updates every 250ms (4x per second)
- **Charts**: Canvas-based history tracking (60-second rolling window)
  - Stacked core chart (all cores as percentage of total CPU)
  - Memory percentage line chart
  - Network RX/TX chart with dynamic max scaling
- **Docker Panel**: Shows top 25 containers, sortable by CPU or memory usage (click column headers)
- **Header Info**: Displays system uptime and local date/time
- **Responsive**: 3-column grid on desktop (CPU/Memory/Network on top, Docker below), stacks on mobile
- **Color coding**: CPU panel red, Memory/Network panels green, Docker status with yellow warnings (70%+) and red alerts (90%+)

## Important Implementation Details

1. **Docker monitoring**:
   - Backend: Connects to the Docker daemon's unix socket directly (`docker_http_get()` — a minimal hand-rolled HTTP/1.1 client: connects, sends a raw GET, reads until EOF via `Connection: close`, de-chunks if needed), polled from a dedicated background thread (`docker_stats_thread`), not inline per-request
   - Lists container IDs from `GET /containers/json?all=false`, then for each one calls `GET /containers/<id>/stats?stream=false` (a single-shot snapshot that conveniently includes both `cpu_stats` and `precpu_stats`, so no separate warm-up sample is needed)
   - CPU%/mem% computed the same way the `docker stats` CLI does internally (cpu delta / system delta * online cpus * 100; usage / limit * 100)
   - Extracts fields via `json_get_number_bounded()` / `json_get_string()` (`strstr`-based lookups scoped to each JSON sub-object by the API's documented field order, not a general JSON parser)
   - Container name: truncated to 20 chars (matching the old Python slice)
   - Returns top 25 sorted by CPU percent descending (`qsort` + `cmp_docker`)
   - Gracefully returns empty array if Docker unavailable (no crash) — socket-connect failures, non-200 responses, and parse failures are all swallowed
   - Frontend: Client-side sorting by CPU or memory via JavaScript event listeners on column headers
   - Sort direction toggled on repeated clicks (arrows: ▲=ascending, ▼=descending)

2. **CPU metrics**:
   - Computed from successive `/proc/stat` samples (`cpu_percent_calc()`), mirroring `psutil.cpu_percent(interval=None)`
   - Per-core percentages calculated separately from the aggregate `cpu` line and each `cpuN` line
   - Global previous-sample state (`g_cpu_prev`, `g_cpu_have_prev`) guarded by `g_lock`; `/stream` explicitly warms it up (and sleeps 500ms) before the first SSE message, matching the old warm-up call

2. **Uptime tracking**:
   - Backend sends `boot_time` (milliseconds), read from the `btime` line in `/proc/stat`
   - Frontend calculates uptime: `(Date.now() - boot_time) / 1000` seconds
   - Formatted as "Xd Yh Zm Zs" in the header
   - Updated every 1 second (separate from metrics refresh)

3. **Network rates**:
   - Stored in global state: `prev_net` (counters, from `/proc/net/dev`) and `prev_net_ts` (monotonic timestamp), guarded by `g_lock`
   - Rate = `(current - previous) / elapsed_seconds`
   - Guard against dt<=0 by clamping `dt` to 1

4. **No temperature reading**: the container image only ever runs on Linux, so this was intentionally not ported — see Backend architecture notes above.

5. **Canvas charts**:
   - Use `ResizeObserver` on the parent container, not the canvas
   - Set `canvas.width/height` in physical pixels, scale by `devicePixelRatio`
   - All datasets maintain a fixed history of 60 entries (shift/push pattern)

6. **CSS Design**:
   - Colors defined as CSS variables (--green, --amber, --text-dim, etc.)
   - Bright panel backgrounds: CPU #3d1515 (red), Memory/Network #0f2710 (green)
   - Brighter text-dim: #66d966 (was #4a8a5a) for better readability
   - Scanline effect: `repeating-linear-gradient` with 2px transparent, 2px semi-transparent black
   - Vignette: `radial-gradient` at 55% radius with increasing darkness
   - Docker container names: Light green #88ff88 with subtle glow
   - Column header sorting: Click to toggle sort, visual indicators (▲/▼)

## Development Notes

- **No database or state persistence**: All metrics are computed on-demand; no storage
- **No authentication**: Assumes running on trusted network (localhost or internal)
- **Docker environment variables**: `ENV_HOST_PROC` is read directly by `app.c` and prefixed onto `/proc` paths when running containerized; `ENV_HOST_SYS` is set for parity with the old Python version but currently unused (no `/sys` reads in the C backend); `ENV_DOCKER_SOCK` optionally overrides the Docker daemon socket path (default `/var/run/docker.sock`)
- **Platform**: Linux only — the binary is compiled for and only runs on Linux
- **SSE reconnection**: Frontend retries every 3 seconds on disconnect
- **Update frequency**: Backend sends metrics every 250ms (4 times per second) for smooth animations

## Hosting & Deployment

Docker Compose configuration:
- Mounts host's `/proc`, `/sys`, and `/etc/os-release` as read-only volumes for system metrics
- Docker daemon socket is accessible for `docker stats` command to enumerate containers
- The `pid: host` setting in compose ensures the container's `/proc` view includes host processes and CPUs
- `ENV_HOST_PROC` points `app.c`'s `/proc` reads at the host filesystem path
