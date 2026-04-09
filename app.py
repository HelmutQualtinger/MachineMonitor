import asyncio
import json
import os
import platform
import time

# Point psutil to host procfs/sysfs when running in Docker
if os.environ.get("ENV_HOST_PROC"):
    os.environ["PSUTIL_HOST_PROC"] = os.environ["ENV_HOST_PROC"]
if os.environ.get("ENV_HOST_SYS"):
    os.environ["PSUTIL_HOST_SYS"] = os.environ["ENV_HOST_SYS"]

import psutil
from fastapi import FastAPI
from fastapi.staticfiles import StaticFiles
from sse_starlette.sse import EventSourceResponse

app = FastAPI()

# ─── Temperature reading via IOKit HID (macOS only) ─────────────────────────
try:
    import objc
    from Foundation import NSBundle

    _iokit_bundle = NSBundle.bundleWithIdentifier_("com.apple.framework.IOKit")
    _hid_funcs = [
        ("IOHIDEventSystemClientCreate", b"@I"),
        ("IOHIDEventSystemClientCopyServices", b"@@"),
        ("IOHIDServiceClientCopyProperty", b"@@@"),
        ("IOHIDServiceClientCopyEvent", b"@@III"),
        ("IOHIDEventGetFloatValue", b"d@I"),
    ]
    _hid_globals = {}
    objc.loadBundleFunctions(_iokit_bundle, _hid_globals, _hid_funcs)

    _kTempType = 15
    _kTempField = 15 << 16
    _HAS_IOKIT = True
except ImportError:
    _HAS_IOKIT = False


def get_temperatures():
    if not _HAS_IOKIT:
        return {}
    client = _hid_globals["IOHIDEventSystemClientCreate"](0)
    services = _hid_globals["IOHIDEventSystemClientCopyServices"](client)
    temps = {}
    for svc in services:
        name = _hid_globals["IOHIDServiceClientCopyProperty"](svc, "Product")
        if not name:
            continue
        event = _hid_globals["IOHIDServiceClientCopyEvent"](svc, _kTempType, 0, 0)
        if event:
            t = _hid_globals["IOHIDEventGetFloatValue"](event, _kTempField)
            if 0 < t < 150:
                temps[str(name)] = round(t, 1)
    return temps

_prev_net = psutil.net_io_counters()
_prev_net_time = time.monotonic()


def get_metrics():
    global _prev_net, _prev_net_time

    cpu_per_core = psutil.cpu_percent(interval=None, percpu=True)
    cpu = sum(cpu_per_core) / len(cpu_per_core) if cpu_per_core else 0

    mem = psutil.virtual_memory()
    swap = psutil.swap_memory()

    now = time.monotonic()
    cur_net = psutil.net_io_counters()
    dt = now - _prev_net_time or 1

    net_sent_rate = (cur_net.bytes_sent - _prev_net.bytes_sent) / dt
    net_recv_rate = (cur_net.bytes_recv - _prev_net.bytes_recv) / dt

    _prev_net = cur_net
    _prev_net_time = now

    return {
        "hostname": platform.node(),
        "ts": int(time.time() * 1000),
        "cpu": {
            "percent": cpu,
            "cores": cpu_per_core,
            "count": psutil.cpu_count(),
        },
        "memory": {
            "percent": mem.percent,
            "used": mem.used,
            "total": mem.total,
            "available": mem.available,
        },
        "swap": {
            "percent": swap.percent,
            "used": swap.used,
            "total": swap.total,
        },
        "network": {
            "sent_rate": net_sent_rate,
            "recv_rate": net_recv_rate,
            "sent_total": cur_net.bytes_sent,
            "recv_total": cur_net.bytes_recv,
        },
    }


async def metrics_generator():
    # Warm up psutil cpu_percent
    psutil.cpu_percent(interval=None, percpu=True)
    await asyncio.sleep(0.5)
    while True:
        yield {"data": json.dumps(get_metrics())}
        await asyncio.sleep(1)


@app.get("/stream")
async def stream():
    return EventSourceResponse(metrics_generator())


@app.get("/metrics")
async def metrics():
    return get_metrics()


app.mount("/", StaticFiles(directory="static", html=True), name="static")
