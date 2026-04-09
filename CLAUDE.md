# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**MachineMonitor** is a real-time system metrics dashboard. It streams CPU, memory, and network statistics to a retro-styled web interface with live charts. Runs locally or in Docker; supports macOS hardware temperature sensors.

## Quick Start

**Local development (macOS):**

```bash
pip install -r requirements.txt
python3 -m uvicorn app:app --reload --port 8000
```

Visit `http://localhost:8000`

**Docker:**

```bash
docker-compose up
```

Runs on `http://localhost:8000` with access to host /proc and /sys.

## Architecture

### Backend (`app.py`)

- **Framework**: FastAPI with uvicorn
- **Metric collection**: Uses psutil for CPU, memory, network stats
- **macOS specific**: Reads hardware temps via IOKit HID (graceful fallback if unavailable)
- **Endpoints**:
  - `/stream` — Server-Sent Events endpoint, yields metrics every 1 second
  - `/metrics` — Single JSON response of current metrics
  - `/` — StaticFiles mount for the dashboard

### Data Structure

Metrics are JSON objects with:

```javascript
{
  hostname: string,
  ts: milliseconds,
  cpu: { percent, cores: [], count },
  memory: { percent, used, total, available },
  swap: { percent, used, total },
  network: { sent_rate, recv_rate, sent_total, recv_total }
}
```

**Key implementation**: Network rates are calculated by tracking previous counter values and computing bytes/second. Global `_prev_net` and `_prev_net_time` track state between requests.

### Frontend (`static/index.html`)

- Single HTML file with inline CSS and vanilla JavaScript
- **Design**: Retro CRT aesthetic (green/cyan on dark), scanlines, vignette
- **Updates**: Connects via EventSource to `/stream` for real-time updates every 1 second
- **Charts**: Canvas-based history tracking (60-second rolling window)
  - Stacked core chart (all cores as percentage of total CPU)
  - Memory percentage line chart
  - Network RX/TX chart with dynamic max scaling
- **Responsive**: 3-column grid on desktop, stacks to single column on mobile
- **Color coding**: Green (normal), amber/yellow (70%+ utilization), red (90%+)

## Important Implementation Details

1. **CPU metrics**:
   - Uses `psutil.cpu_percent(interval=None)` for non-blocking reads
   - Per-core percentages are calculated separately
   - Initialize with a warm-up call before the first SSE message

2. **Network rates**:
   - Stored in global state: `_prev_net` (counters) and `_prev_net_time` (timestamp)
   - Rate = `(current - previous) / elapsed_seconds`
   - Guard against dt=0 with `dt = ... or 1`

3. **macOS temperature reading**:
   - Uses pyobjc to bridge to IOKit HID APIs
   - Enumerable only on macOS; gracefully disabled on other platforms
   - Filtered for realistic values (0–150 °C)

4. **Canvas charts**:
   - Use `ResizeObserver` on the parent container, not the canvas
   - Set `canvas.width/height` in physical pixels, scale by `devicePixelRatio`
   - All datasets maintain a fixed history of 60 entries (shift/push pattern)

5. **CSS Design**:
   - Colors defined as CSS variables (--green, --amber, etc.)
   - Scanline effect: `repeating-linear-gradient` with 2px transparent, 2px semi-transparent black
   - Vignette: `radial-gradient` at 55% radius with increasing darkness

## Development Notes

- **No database or state persistence**: All metrics are computed on-demand; no storage
- **No authentication**: Assumes running on trusted network (localhost or internal)
- **Docker environment variables**: `ENV_HOST_PROC` and `ENV_HOST_SYS` point psutil to containerized host FS
- **Platform differences**: macOS can read temps; Linux/Docker cannot (not a blocker)
- **SSE reconnection**: Frontend retries every 3 seconds on disconnect

## Hosting & Deployment

Docker Compose configuration mounts host's `/proc` and `/sys` as read-only volumes, allowing container to inspect host system metrics. The `pid: host` setting in compose ensures psutil can enumerate host processes and CPUs.
