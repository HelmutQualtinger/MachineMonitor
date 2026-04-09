# MachineMonitor

A real-time system metrics dashboard with a retro CRT aesthetic. Stream live CPU, memory, and network statistics with beautiful interactive charts.

## Features

- **Live Metrics**: CPU usage per core, memory/swap usage, network I/O rates
- **Interactive Charts**: 60-second rolling history with smooth animations
- **Retro Design**: Authentic CRT aesthetic with scanlines, vignette, and glowing green text
- **Responsive**: Works on desktop and mobile
- **macOS Support**: Reads hardware temperatures (with graceful fallback on other platforms)
- **Docker Ready**: Containerized with access to host metrics
- **SSE Stream**: Server-Sent Events for real-time updates without polling

## Quick Start

### Local (macOS/Linux)

```bash
pip install -r requirements.txt
python3 -m uvicorn app:app --reload --port 8000
```

Open http://localhost:8000 in your browser.

### Docker

```bash
docker-compose up
```

Open http://localhost:8000. Runs on port 8000.

## Requirements

- **Python 3.12+** (for local development)
- **Dependencies**: FastAPI, uvicorn, psutil, sse-starlette
- **macOS only**: pyobjc (for hardware temperature reading)

## System Requirements

- **Memory**: Minimal (~20 MB overhead)
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

## Development

See `CLAUDE.md` for architecture details, implementation notes, and development guidance.

### Project Structure

```
MachineMonitor/
├── app.py              # FastAPI backend, metrics collection
├── static/
│   └── index.html      # Dashboard UI (HTML/CSS/JS)
├── Dockerfile          # Container image
├── docker-compose.yml  # Container orchestration
└── requirements.txt    # Python dependencies
```

## API

### GET /metrics

Returns current metrics as JSON.

```json
{
  "hostname": "MacBook-Pro",
  "ts": 1712691234000,
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
  }
}
```

### GET /stream

Server-Sent Events endpoint. Streams metrics every 1 second.

```
data: {"hostname":"MacBook-Pro","ts":1712691234000,...}
```

## Design Notes

- No authentication — assumes trusted network
- No data persistence — metrics computed on-demand
- Single-page application — no build step required
- Canvas rendering for performant charts

## License

MIT
