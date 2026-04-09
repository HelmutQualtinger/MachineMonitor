# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**MachineMonitor** is a real-time system metrics dashboard. It streams CPU, memory, network, and Docker container statistics to a retro-styled web interface with live charts and fast updates (250ms). Runs locally or in Docker; supports macOS hardware temperature sensors.

## Quick Start

**Local development (macOS):**

```bash
pip install -r requirements.txt
python3 -m uvicorn app:app --reload --port 7777
```

Visit `http://localhost:7777`

**Docker:**

```bash
docker-compose up
```

Runs on `http://localhost:7777` with access to host /proc, /sys, and Docker daemon.

## Architecture

### Backend (`app.py`)

- **Framework**: FastAPI with uvicorn
- **Metric collection**: Uses psutil for CPU, memory, network stats
- **macOS specific**: Reads hardware temps via IOKit HID (graceful fallback if unavailable)
- **Endpoints**:
  - `/stream` — Server-Sent Events endpoint, yields metrics every 250ms (4x per second)
  - `/metrics` — Single JSON response of current metrics
  - `/` — StaticFiles mount for the dashboard
- **Docker Integration**: Calls `docker stats --no-stream` to fetch container stats, parsed and sorted by CPU usage (top 25)

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
   - Backend: Uses `subprocess.run()` with ThreadPoolExecutor to avoid blocking async event loop
   - Executes `docker stats --no-stream --format '{{json .}}'` and parses JSON output
   - Extracts: container name, CPU%, memory usage, memory%
   - Returns top 25 sorted by CPU percent descending
   - Gracefully returns empty array if Docker unavailable (no crash)
   - Frontend: Client-side sorting by CPU or memory via JavaScript event listeners on column headers
   - Sort direction toggled on repeated clicks (arrows: ▲=ascending, ▼=descending)

2. **CPU metrics**:
   - Uses `psutil.cpu_percent(interval=None)` for non-blocking reads
   - Per-core percentages are calculated separately
   - Initialize with a warm-up call before the first SSE message

2. **Uptime tracking**:
   - Backend sends `boot_time` (milliseconds) from `psutil.boot_time()`
   - Frontend calculates uptime: `(Date.now() - boot_time) / 1000` seconds
   - Formatted as "Xd Yh Zm Zs" in the header
   - Updated every 1 second (separate from metrics refresh)

3. **Network rates**:
   - Stored in global state: `_prev_net` (counters) and `_prev_net_time` (timestamp)
   - Rate = `(current - previous) / elapsed_seconds`
   - Guard against dt=0 with `dt = ... or 1`

4. **macOS temperature reading**:
   - Uses pyobjc to bridge to IOKit HID APIs
   - Enumerable only on macOS; gracefully disabled on other platforms
   - Filtered for realistic values (0–150 °C)

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
- **Docker environment variables**: `ENV_HOST_PROC` and `ENV_HOST_SYS` point psutil to containerized host FS
- **Platform differences**: macOS can read temps; Linux/Docker cannot (not a blocker)
- **SSE reconnection**: Frontend retries every 3 seconds on disconnect
- **Update frequency**: Backend sends metrics every 250ms (4 times per second) for smooth animations

## Hosting & Deployment

Docker Compose configuration:
- Mounts host's `/proc`, `/sys`, and `/etc/os-release` as read-only volumes for system metrics
- Docker daemon socket is accessible for `docker stats` command to enumerate containers
- The `pid: host` setting in compose ensures psutil can enumerate host processes and CPUs
- Environment variables `ENV_HOST_PROC` and `ENV_HOST_SYS` point psutil to host filesystem paths
