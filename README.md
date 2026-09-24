# MachineMonitor

A real-time system metrics dashboard with a retro CRT aesthetic. Stream live CPU, memory, network, and Docker container statistics with beautiful interactive charts. Fast updates (4x per second) for smooth animations.

## Features

- **Live Metrics**: CPU usage per core, memory/swap usage, network I/O rates
- **Docker Monitoring**: Top 25 containers by CPU/memory with sortable columns
- **Interactive Charts**: 60-second rolling history with smooth animations
- **Fast Updates**: 250ms refresh rate for near real-time responsiveness
- **Retro Design**: Authentic CRT aesthetic with scanlines, vignette, and glowing text
- **System Info**: Displays uptime and local date/time in header
- **Responsive**: Works on desktop and mobile
- **Docker Ready**: Containerized with access to host metrics and the Docker daemon socket
- **SSE Stream**: Server-Sent Events for real-time updates without polling

## Quick Start

### Local (Linux)

```bash
gcc -O2 -Wall -o machinemonitor app.c -lpthread
./machinemonitor
```

Open http://localhost:8000 in your browser (set `ENV_HOST_PROC` to point at an alternate procfs).

### Docker

```bash
docker-compose up
```

Open http://localhost:7777. Runs on port 7777.

## Requirements

- **Platform**: Linux only (reads `/proc` directly; the binary only runs on Linux)
- **Build**: a C11 compiler and `libpthread` — no other dependencies
- **Docker monitoring**: access to a Docker daemon socket (default `/var/run/docker.sock`); optional, fails gracefully if unavailable

## System Requirements

- **Memory**: Minimal (a few MB overhead)
- **CPU**: Negligible impact
- **Network**: None required (local monitoring only by default)

## Dashboard Overview

### CPU Panel
- Aggregate CPU percentage with real-time gauge
- Per-core breakdown (grid layout)
- 60-second history chart (stacked area)
- Color-coded alerts: green (normal), amber (70%+), red (90%+)

### Memory Panel
- Memory percentage and absolute usage
- Swap usage with separate gauge
- Used/free/total breakdown
- Historical chart

### Network Panel
- Download (▼) and upload (▲) rates
- Total RX/TX counters
- 60-second history chart

### Docker Monitoring Panel
- Top 25 containers sorted by CPU or memory usage
- Click "CPU" or "MEM" column header to sort (▲/▼ indicators)
- Real-time updates of container resource usage
- Color-coded alerts: green (normal), yellow (70%+), red (90%+)

## Development

See `CLAUDE.md` for architecture details, implementation notes, and development guidance.

### Project Structure

```
MachineMonitor/
├── app.c                # C backend: HTTP server, metrics collection, Docker stats
├── static/
│   └── index.html       # Dashboard UI (HTML/CSS/JS)
├── Dockerfile           # Container image (multi-stage Alpine build)
├── docker-compose.yml   # Container orchestration
├── install-service.sh   # Helper to install as a systemd service
└── docker-top-containers.sh  # Helper script
```

## API

### GET /metrics

Returns current metrics as JSON.

```json
{
  "hostname": "my-vps",
  "ts": 1712691234000,
  "boot_time": 1712604834000,
  "cpu": {
    "percent": 45.2,
    "cores": [30.1, 50.5, 40.3, 55.2],
    "count": 4
  },
  "memory": {
    "percent": 62.3,
    "used": 10737418240,
    "total": 17179869184,
    "available": 6442450944
  },
  "swap": {
    "percent": 0.0,
    "used": 0,
    "total": 0
  },
  "network": {
    "sent_rate": 1048576,
    "recv_rate": 2097152,
    "sent_total": 1073741824,
    "recv_total": 2147483648
  },
  "docker": [
    {
      "name": "container-name",
      "cpu_percent": 23.5,
      "mem_usage": "256MiB / 7.706GiB",
      "mem_percent": 3.24
    }
  ]
}
```

### GET /stream

Server-Sent Events endpoint. Streams metrics every 250ms (4 times per second).

```
data: {"hostname":"my-vps","ts":1712691234000,"docker":[...],...}
```

## Environment Variables

- `ENV_HOST_PROC` — prefixed onto `/proc` reads (used when running containerized against a mounted host procfs)
- `ENV_HOST_SYS` — set for parity with the old Python version; currently unused (no `/sys` reads in the C backend)
- `ENV_DOCKER_SOCK` — overrides the Docker daemon socket path (default `/var/run/docker.sock`)

## Design Notes

- No authentication — assumes trusted network
- No data persistence — metrics computed on-demand
- Single-page application — no build step required
- Canvas rendering for performant charts
- Docker stats are fetched by talking directly to the Docker daemon's HTTP API over its unix socket — no `docker` CLI binary is bundled in the image

## License

MIT
