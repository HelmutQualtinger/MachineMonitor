# MachineMonitor

A real-time system metrics dashboard styled like a 1970s submarine control room — riveted steel panels, amber/green LCD instrument readouts. Stream live CPU, memory, network, host process, and Docker container statistics with interactive charts. Fast updates (4x per second) for smooth animations.

> No screenshot yet — see [Dashboard Overview](#dashboard-overview) below for what each panel shows, or run it yourself (`docker-compose up`, then open http://localhost:7777).

## Features

- **Live Metrics**: CPU usage per core, memory/swap usage, network I/O rates
- **Docker Monitoring**: Top 25 containers by CPU/memory with sortable columns
- **Host Process Monitoring**: Top 25 host processes by CPU/memory with sortable columns, shown next to the Docker panel
- **Interactive Charts**: 60-second rolling history with smooth animations
- **Fast Updates**: 250ms refresh rate for near real-time responsiveness
- **Retro Design**: Riveted steel control-panel look — film grain, amber/green LCD readouts, segmented LED gauges. Toggle in the header switches to a second "Bugatti cockpit" theme (carbon fiber, ice-blue/red), same as strom.bekerh.ddns.net
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

## Security

**This container effectively has root-level access to the host it runs on — this is not accidental, but it's important to understand before you deploy it anywhere.** To show real host metrics (not just the container's own isolated view), the Docker Compose setup deliberately breaks Docker's normal container isolation:

- **`pid: host`** — the container shares the host's process namespace, so it can see (and read `/proc/<pid>/environ` and command lines for) *every* process on the host, not just its own.
- **Host `/proc` and `/sys` mounted read-only** — read-only stops the container from *writing* to host process/kernel state, but reading host-wide metrics was the point.
- **Docker socket mounted** — this is the one to pay attention to. Even mounted `:ro`, that flag only stops the container from replacing the socket *file*; it does nothing to limit what you can do once connected to it. Anyone who can reach that socket can ask the Docker API to start a brand-new container with full host filesystem access and `--privileged` — a direct path to root on the host, regardless of the `:ro` flag.

**In short**: a security bug in this app (or anything with network access to its port) is a security bug in your host, not just in a sandboxed container. Treat it accordingly:
- Never expose port 7777 to an untrusted network or the public internet.
- Run it only on a trusted LAN or behind a reverse proxy with authentication, if it must be reachable beyond localhost.
- This tradeoff is the same one cAdvisor, netdata, and node-exporter make — it's normal for this class of tool, just not something to deploy casually on an internet-facing box.

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

### Host Processes Panel
- Top 25 host processes sorted by CPU or memory usage
- Click "CPU" or "MEM" column header to sort (▲/▼ indicators)
- Shows PID, name, CPU%, and resident memory
- Positioned to the left of the Docker panel

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
  ],
  "processes": [
    {
      "pid": 1234,
      "name": "nginx",
      "cpu_percent": 12.3,
      "mem": 104857600,
      "mem_percent": 1.22
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

- No authentication — assumes trusted network (see [Security](#security) above)
- No data persistence — metrics computed on-demand
- Single-page application — no build step required
- Canvas rendering for performant charts
- Docker stats are fetched by talking directly to the Docker daemon's HTTP API over its unix socket — no `docker` CLI binary is bundled in the image
- Network rate only counts real NICs — loopback and Docker's own internal bridges/veth interfaces are excluded, since traffic between containers otherwise gets counted multiple times over (once per veth, once per bridge) and would report a rate far higher than any actual external bandwidth

## License

MIT
