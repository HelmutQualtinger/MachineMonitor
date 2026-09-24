/*
 * MachineMonitor backend (C translation of app.py)
 *
 * Serves:
 *   GET /stream   Server-Sent Events, one metrics JSON blob every 250ms
 *   GET /metrics  Single JSON metrics response
 *   GET /...      Static files from ./static (index.html at "/")
 *
 * Metrics are read directly from /proc (or $ENV_HOST_PROC when running in
 * Docker with the host procfs bind-mounted). Docker container stats come
 * from the Docker daemon's own HTTP API, spoken directly over its unix
 * socket ($ENV_DOCKER_SOCK, default /var/run/docker.sock) — no `docker`
 * CLI binary required.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define PORT 8000
#define MAX_CORES 512
#define STATIC_DIR "static"
#define MAX_DOCKER 25

static char host_proc[512] = "/proc";

/* ------------------------------------------------------------------ */
/* growable string buffer                                             */
/* ------------------------------------------------------------------ */

typedef struct { char *buf; size_t len, cap; } sbuf_t;

static void sbuf_init(sbuf_t *s) {
    s->cap = 4096;
    s->len = 0;
    s->buf = malloc(s->cap);
    s->buf[0] = 0;
}

static void sbuf_free(sbuf_t *s) { free(s->buf); }

static void sbuf_appendf(sbuf_t *s, const char *fmt, ...) {
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int need = vsnprintf(s->buf + s->len, s->cap - s->len, fmt, ap);
        va_end(ap);
        if (need < 0) return;
        if ((size_t)need < s->cap - s->len) { s->len += (size_t)need; return; }
        s->cap = (s->cap + (size_t)need + 1) * 2;
        s->buf = realloc(s->buf, s->cap);
    }
}

static void json_escape_append(sbuf_t *s, const char *str) {
    sbuf_appendf(s, "\"");
    for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
        if (*p == '"' || *p == '\\') sbuf_appendf(s, "\\%c", *p);
        else if (*p < 0x20) sbuf_appendf(s, "\\u%04x", *p);
        else sbuf_appendf(s, "%c", *p);
    }
    sbuf_appendf(s, "\"");
}

/* ------------------------------------------------------------------ */
/* CPU percent (per core + overall), mirrors psutil.cpu_percent       */
/* ------------------------------------------------------------------ */

typedef struct {
    long long user, nice, sys, idle, iowait, irq, softirq, steal;
} cpu_times_t;

/* index 0 = aggregate "cpu" line, 1..n = per-core "cpuN" lines */
static cpu_times_t g_cpu_prev[MAX_CORES + 1];
static int g_cpu_have_prev = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int read_cpu_times(cpu_times_t *out, int max_slots, int *ncores) {
    char path[600];
    snprintf(path, sizeof(path), "%s/stat", host_proc);
    FILE *f = fopen(path, "r");
    if (!f) { *ncores = 0; return -1; }

    char line[512];
    int idx = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu", 3) != 0) break;
        char label[16];
        cpu_times_t t;
        memset(&t, 0, sizeof(t));
        int n = sscanf(line, "%15s %lld %lld %lld %lld %lld %lld %lld %lld",
                        label, &t.user, &t.nice, &t.sys, &t.idle,
                        &t.iowait, &t.irq, &t.softirq, &t.steal);
        if (n < 5) continue;
        if (strcmp(label, "cpu") == 0) {
            out[0] = t;
        } else {
            idx++;
            if (idx < max_slots) out[idx] = t;
        }
    }
    fclose(f);
    *ncores = idx;
    return 0;
}

static double cpu_percent_calc(const cpu_times_t *prev, const cpu_times_t *cur) {
    long long prev_idle = prev->idle + prev->iowait;
    long long cur_idle = cur->idle + cur->iowait;
    long long prev_non = prev->user + prev->nice + prev->sys + prev->irq + prev->softirq + prev->steal;
    long long cur_non = cur->user + cur->nice + cur->sys + cur->irq + cur->softirq + cur->steal;
    long long prev_total = prev_idle + prev_non;
    long long cur_total = cur_idle + cur_non;
    long long dt = cur_total - prev_total;
    long long didle = cur_idle - prev_idle;
    if (dt <= 0) return 0.0;
    double pct = (double)(dt - didle) / (double)dt * 100.0;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

/* ------------------------------------------------------------------ */
/* memory / swap                                                      */
/* ------------------------------------------------------------------ */

typedef struct { double percent; long long used, total, available; } mem_t;
typedef struct { double percent; long long used, total; } swap_t;

static void read_meminfo(mem_t *mem, swap_t *swap) {
    char path[600];
    snprintf(path, sizeof(path), "%s/meminfo", host_proc);
    FILE *f = fopen(path, "r");

    long long memtotal = 0, memfree = 0, memavail = -1, buffers = 0;
    long long cached = 0, sreclaim = 0, shmem = 0, swaptotal = 0, swapfree = 0;

    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char key[64];
            long long val;
            if (sscanf(line, "%63[^:]: %lld", key, &val) == 2) {
                val *= 1024; /* kB -> bytes */
                if (!strcmp(key, "MemTotal")) memtotal = val;
                else if (!strcmp(key, "MemFree")) memfree = val;
                else if (!strcmp(key, "MemAvailable")) memavail = val;
                else if (!strcmp(key, "Buffers")) buffers = val;
                else if (!strcmp(key, "Cached")) cached = val;
                else if (!strcmp(key, "SReclaimable")) sreclaim = val;
                else if (!strcmp(key, "Shmem")) shmem = val;
                else if (!strcmp(key, "SwapTotal")) swaptotal = val;
                else if (!strcmp(key, "SwapFree")) swapfree = val;
            }
        }
        fclose(f);
    }

    long long cached_total = cached + sreclaim - shmem;
    if (cached_total < 0) cached_total = 0;
    long long avail = memavail >= 0 ? memavail : (memfree + buffers + cached_total);
    long long used = memtotal - memfree - buffers - cached_total;
    if (used < 0) used = memtotal - memfree;

    mem->total = memtotal;
    mem->available = avail;
    mem->used = used;
    mem->percent = memtotal > 0 ? (double)(memtotal - avail) / (double)memtotal * 100.0 : 0.0;

    swap->total = swaptotal;
    swap->used = swaptotal - swapfree;
    swap->percent = swaptotal > 0 ? (double)swap->used / (double)swaptotal * 100.0 : 0.0;
}

/* ------------------------------------------------------------------ */
/* network                                                             */
/* ------------------------------------------------------------------ */

typedef struct { long long sent, recv; } net_t;

static void read_net(net_t *out) {
    char path[600];
    snprintf(path, sizeof(path), "%s/net/dev", host_proc);
    out->sent = 0;
    out->recv = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    fgets(line, sizeof(line), f); /* header */
    fgets(line, sizeof(line), f); /* header */
    while (fgets(line, sizeof(line), f)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        long long vals[16];
        int n = 0;
        char *tok = strtok(colon + 1, " \t\n");
        while (tok && n < 16) { vals[n++] = atoll(tok); tok = strtok(NULL, " \t\n"); }
        if (n >= 9) {
            out->recv += vals[0];
            out->sent += vals[8];
        }
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* boot time / hostname                                               */
/* ------------------------------------------------------------------ */

static long long read_boot_time(void) {
    char path[600];
    snprintf(path, sizeof(path), "%s/stat", host_proc);
    FILE *f = fopen(path, "r");
    long long bt = 0;
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "btime", 5)) { sscanf(line, "btime %lld", &bt); break; }
        }
        fclose(f);
    }
    return bt;
}

/* ------------------------------------------------------------------ */
/* docker stats — spoken directly to the daemon's HTTP API over its    */
/* unix socket, no `docker` CLI binary involved                        */
/* ------------------------------------------------------------------ */

static int send_all(int fd, const char *data, size_t len); /* defined in HTTP server section below */

typedef struct {
    char name[32];
    double cpu_percent;
    char mem_usage[80];
    double mem_percent;
} docker_c_t;

static int json_get_string(const char *line, const char *key, char *out, size_t outsz) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(line, pat);
    if (!p) return 0;
    p += strlen(pat);
    const char *end = strchr(p, '"');
    if (!end) return 0;
    size_t len = (size_t)(end - p);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, p, len);
    out[len] = 0;
    return 1;
}

/*
 * Looks up a bare (unquoted) numeric field "key":123 within [start, end).
 * `end` bounds the search to one JSON sub-object so that e.g. cpu_stats's
 * "total_usage" isn't confused with precpu_stats's field of the same name.
 * `[start, end)` is expected to come from json_find_value() below.
 */
static int json_get_number_bounded(const char *start, const char *end, const char *key, double *out) {
    if (!start) return 0;
    char pat[64];
    int patlen = snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(start, pat);
    if (!p || p >= end) return 0;
    *out = atof(p + patlen);
    return 1;
}

/*
 * Finds the value of "key": in `hay` and returns a pointer to where it
 * starts, with *value_end set to one past where it ends. Object/array
 * values are matched by real brace/bracket depth-counting (skipping over
 * quoted strings, including escapes) rather than by assuming any
 * particular key order — Docker API versions have shuffled the order of
 * cpu_stats/precpu_stats/memory_stats before, so that assumption doesn't
 * hold across daemon versions. Still not a general JSON parser: no
 * unicode-escape decoding, numbers/literals are simply scanned up to the
 * next `,`/`}`/`]`.
 */
static const char *json_find_value(const char *hay, const char *key, const char **value_end) {
    char pat[64];
    int patlen = snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(hay, pat);
    if (!p) return NULL;
    p += patlen;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    const char *start = p;

    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        int depth = 0;
        for (; *p; p++) {
            if (*p == '"') {
                p++;
                while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
                continue;
            }
            if (*p == open) depth++;
            else if (*p == close && --depth == 0) { p++; break; }
        }
    } else if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
        if (*p == '"') p++;
    } else {
        while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    }
    *value_end = p;
    return start;
}

static int cmp_docker(const void *a, const void *b) {
    const docker_c_t *da = a, *db = b;
    if (db->cpu_percent > da->cpu_percent) return 1;
    if (db->cpu_percent < da->cpu_percent) return -1;
    return 0;
}

static void format_bytes(double v, char *out, size_t outsz) {
    static const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    int i = 0;
    while (v >= 1024.0 && i < 5) { v /= 1024.0; i++; }
    if (i == 0) snprintf(out, outsz, "%.0f%s", v, units[i]);
    else if (v < 10) snprintf(out, outsz, "%.2f%s", v, units[i]);
    else if (v < 100) snprintf(out, outsz, "%.1f%s", v, units[i]);
    else snprintf(out, outsz, "%.0f%s", v, units[i]);
}

static const char *docker_sock_path(void) {
    const char *p = getenv("ENV_DOCKER_SOCK");
    return (p && *p) ? p : "/var/run/docker.sock";
}

static int docker_sock_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    /*
     * A single /stats?stream=false call routinely takes ~1-2s on its own
     * (the daemon samples cgroup counters over a short window); leave
     * headroom above that so a legitimately slow-but-alive daemon doesn't
     * get mistaken for a hung one under concurrent load.
     */
    struct timeval tv = { .tv_sec = 8, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, docker_sock_path(), sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    return fd;
}

typedef struct { char *data; size_t len; } raw_buf_t;

static int recv_all(int fd, raw_buf_t *out) {
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) return -1;
    for (;;) {
        if (len + 4097 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); return -1; }
            buf = nb;
        }
        ssize_t n = recv(fd, buf + len, cap - len - 1, 0);
        if (n <= 0) break;
        len += (size_t)n;
    }
    buf[len] = 0;
    out->data = buf;
    out->len = len;
    return 0;
}

/* Decodes an HTTP chunked-transfer-encoded body in place into a new buffer. */
static char *dechunk(const char *body, size_t body_len, size_t *out_len) {
    char *out = malloc(body_len + 1);
    size_t olen = 0;
    const char *p = body, *end = body + body_len;
    while (p < end) {
        const char *eol = memchr(p, '\r', (size_t)(end - p));
        if (!eol || eol + 1 >= end) break;
        long csize = strtol(p, NULL, 16);
        if (csize <= 0) break;
        p = eol + 2;
        if (p + csize > end) break;
        memcpy(out + olen, p, (size_t)csize);
        olen += (size_t)csize;
        p += csize + 2;
    }
    out[olen] = 0;
    *out_len = olen;
    return out;
}

/*
 * GET over the Docker daemon's unix socket. Returns a malloc'd,
 * null-terminated response body on 200 OK, or NULL on any failure (socket
 * missing/unreachable, non-200 status, malformed response, ...) — callers
 * treat that the same as "Docker unavailable", matching the old popen
 * behaviour of swallowing failures rather than crashing.
 */
static char *docker_http_get(const char *path) {
    int fd = docker_sock_connect();
    if (fd < 0) return NULL;

    char req[512];
    int rl = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: docker\r\nConnection: close\r\nAccept: application/json\r\n\r\n",
        path);
    if (rl < 0 || send_all(fd, req, (size_t)rl) < 0) { close(fd); return NULL; }

    raw_buf_t raw;
    if (recv_all(fd, &raw) < 0) { close(fd); return NULL; }
    close(fd);

    if (strncmp(raw.data, "HTTP/1.1 200", 12) != 0) { free(raw.data); return NULL; }

    char *hdr_end = strstr(raw.data, "\r\n\r\n");
    if (!hdr_end) { free(raw.data); return NULL; }
    char *body = hdr_end + 4;
    size_t body_len = raw.len - (size_t)(body - raw.data);

    char saved = *hdr_end;
    *hdr_end = 0;
    int chunked = strcasestr(raw.data, "Transfer-Encoding: chunked") != NULL;
    *hdr_end = saved;

    char *result;
    if (chunked) {
        size_t dlen;
        result = dechunk(body, body_len, &dlen);
    } else {
        result = malloc(body_len + 1);
        if (result) { memcpy(result, body, body_len); result[body_len] = 0; }
    }
    free(raw.data);
    return result;
}

#define MAX_DOCKER_IDS 256

static int list_container_ids(char ids[][80], int max) {
    char *body = docker_http_get("/containers/json?all=false");
    if (!body) return 0;

    int n = 0;
    const char *p = body;
    while (n < max) {
        const char *m = strstr(p, "\"Id\":\"");
        if (!m) break;
        m += 6;
        const char *e = strchr(m, '"');
        if (!e) break;
        size_t len = (size_t)(e - m);
        if (len >= 80) len = 79;
        memcpy(ids[n], m, len);
        ids[n][len] = 0;
        n++;
        p = e + 1;
    }
    free(body);
    return n;
}

/*
 * Fetches one container's stats via /containers/<id>/stats?stream=false,
 * which — unlike the streaming variant — returns cpu_stats *and*
 * precpu_stats (a real previous sample the daemon already had cached) in a
 * single response, so no separate warm-up round trip is needed. CPU% and
 * mem% are computed the same way the `docker stats` CLI itself does.
 */
static int fetch_container_stat(const char *id, docker_c_t *out) {
    char path[128];
    snprintf(path, sizeof(path), "/containers/%.79s/stats?stream=false", id);
    char *body = docker_http_get(path);
    if (!body) return 0;

    const char *cpu_e, *precpu_e, *mem_e;
    const char *cpu_s = json_find_value(body, "cpu_stats", &cpu_e);
    const char *precpu_s = json_find_value(body, "precpu_stats", &precpu_e);
    const char *mem_s = json_find_value(body, "memory_stats", &mem_e);
    if (!cpu_s || !precpu_s || !mem_s) {
        free(body);
        return 0;
    }
    const char *name_s = strstr(body, "\"name\":");

    double cpu_total = 0, cpu_sys = 0, online = 0;
    double precpu_total = 0, precpu_sys = 0;
    double mem_usage = 0, mem_limit = 0;

    json_get_number_bounded(cpu_s, cpu_e, "total_usage", &cpu_total);
    json_get_number_bounded(cpu_s, cpu_e, "system_cpu_usage", &cpu_sys);
    json_get_number_bounded(cpu_s, cpu_e, "online_cpus", &online);
    json_get_number_bounded(precpu_s, precpu_e, "total_usage", &precpu_total);
    json_get_number_bounded(precpu_s, precpu_e, "system_cpu_usage", &precpu_sys);
    json_get_number_bounded(mem_s, mem_e, "usage", &mem_usage);
    json_get_number_bounded(mem_s, mem_e, "limit", &mem_limit);

    double cpu_delta = cpu_total - precpu_total;
    double sys_delta = cpu_sys - precpu_sys;
    double cpu_pct = 0.0;
    if (sys_delta > 0 && cpu_delta > 0) cpu_pct = (cpu_delta / sys_delta) * (online > 0 ? online : 1.0) * 100.0;

    char name[64] = {0};
    json_get_string(name_s ? name_s : body, "name", name, sizeof(name));
    const char *disp_name = name[0] == '/' ? name + 1 : name;

    memset(out, 0, sizeof(*out));
    size_t nlen = strlen(disp_name);
    if (nlen > sizeof(out->name) - 1) nlen = sizeof(out->name) - 1;
    memcpy(out->name, disp_name, nlen);
    out->name[nlen] = 0;
    out->name[20] = 0; /* mirror python's [:20] truncation */
    out->cpu_percent = cpu_pct;
    out->mem_percent = mem_limit > 0 ? mem_usage / mem_limit * 100.0 : 0.0;

    char a[32], b[32];
    format_bytes(mem_usage, a, sizeof(a));
    format_bytes(mem_limit, b, sizeof(b));
    snprintf(out->mem_usage, sizeof(out->mem_usage), "%.31s / %.31s", a, b);

    free(body);
    return 1;
}

typedef struct {
    char id[80];
    docker_c_t result;
    int ok;
} docker_fetch_task_t;

static void *fetch_thread(void *arg) {
    docker_fetch_task_t *t = arg;
    t->ok = fetch_container_stat(t->id, &t->result);
    return NULL;
}

/*
 * `/containers/<id>/stats?stream=false` isn't actually cheap per call — it
 * routinely takes ~1-2s even for a single container (the daemon samples
 * cgroup counters over a short internal window). Fetched serially, N
 * containers would take N * ~1.5s, which blows well past any reasonable
 * poll interval. So every container is fetched from its own thread at
 * once, same as how the `docker stats` CLI itself streams all containers
 * concurrently — total wall time stays close to that of the single
 * slowest container instead of the sum of all of them.
 */
static int get_docker_stats(docker_c_t *out, int max) {
    char ids[MAX_DOCKER_IDS][80];
    int nids = list_container_ids(ids, MAX_DOCKER_IDS);
    if (nids <= 0) return 0;

    docker_fetch_task_t tasks[MAX_DOCKER_IDS];
    pthread_t threads[MAX_DOCKER_IDS];
    for (int i = 0; i < nids; i++) {
        memcpy(tasks[i].id, ids[i], sizeof(tasks[i].id) - 1);
        tasks[i].id[sizeof(tasks[i].id) - 1] = 0;
        tasks[i].ok = 0;
        if (pthread_create(&threads[i], NULL, fetch_thread, &tasks[i]) != 0) {
            fetch_thread(&tasks[i]); /* fall back to inline on thread-creation failure */
            threads[i] = 0;
        }
    }
    for (int i = 0; i < nids; i++) {
        if (threads[i]) pthread_join(threads[i], NULL);
    }

    docker_c_t items[MAX_DOCKER_IDS];
    int n = 0;
    for (int i = 0; i < nids; i++) {
        if (tasks[i].ok) items[n++] = tasks[i].result;
    }

    qsort(items, (size_t)n, sizeof(docker_c_t), cmp_docker);
    int cnt = n < max ? n : max;
    memcpy(out, items, (size_t)cnt * sizeof(docker_c_t));
    return cnt;
}

/*
 * Even parallelized, a full listing + concurrent per-container stats pass
 * takes noticeably longer than the 250ms SSE tick (and blocks up to the
 * per-request socket timeout if the daemon stalls), so it isn't safe to
 * run inline from every SSE tick. A single background thread polls it in
 * a loop (one pass at a time, ever) and every request just reads the
 * cached result.
 */
static docker_c_t g_docker_cache[MAX_DOCKER];
static int g_docker_count = 0;
static pthread_mutex_t g_docker_lock = PTHREAD_MUTEX_INITIALIZER;

static void *docker_stats_thread(void *arg) {
    (void)arg;
    for (;;) {
        docker_c_t tmp[MAX_DOCKER];
        int n = get_docker_stats(tmp, MAX_DOCKER);
        pthread_mutex_lock(&g_docker_lock);
        memcpy(g_docker_cache, tmp, (size_t)n * sizeof(docker_c_t));
        g_docker_count = n;
        pthread_mutex_unlock(&g_docker_lock);
        sleep(1);
    }
    return NULL;
}

static int get_docker_stats_cached(docker_c_t *out, int max) {
    pthread_mutex_lock(&g_docker_lock);
    int n = g_docker_count < max ? g_docker_count : max;
    memcpy(out, g_docker_cache, (size_t)n * sizeof(docker_c_t));
    pthread_mutex_unlock(&g_docker_lock);
    return n;
}

/* ------------------------------------------------------------------ */
/* metrics JSON assembly                                               */
/* ------------------------------------------------------------------ */

static void build_metrics_json(sbuf_t *s) {
    static net_t prev_net;
    static struct timespec prev_net_ts;
    static int have_prev_net = 0;

    pthread_mutex_lock(&g_lock);

    cpu_times_t cur[MAX_CORES + 1];
    int ncores = 0;
    read_cpu_times(cur, MAX_CORES + 1, &ncores);
    if (ncores > MAX_CORES) ncores = MAX_CORES;

    double per_core[MAX_CORES];
    double cpu_overall;
    if (g_cpu_have_prev) {
        for (int i = 0; i < ncores; i++) per_core[i] = cpu_percent_calc(&g_cpu_prev[i + 1], &cur[i + 1]);
        cpu_overall = cpu_percent_calc(&g_cpu_prev[0], &cur[0]);
    } else {
        for (int i = 0; i < ncores; i++) per_core[i] = 0.0;
        cpu_overall = 0.0;
    }
    memcpy(g_cpu_prev, cur, sizeof(cpu_times_t) * (size_t)(ncores + 1));
    g_cpu_have_prev = 1;

    mem_t mem; swap_t swap;
    read_meminfo(&mem, &swap);

    net_t cur_net; read_net(&cur_net);
    struct timespec now_ts; clock_gettime(CLOCK_MONOTONIC, &now_ts);
    double sent_rate = 0, recv_rate = 0;
    if (have_prev_net) {
        double dt = (double)(now_ts.tv_sec - prev_net_ts.tv_sec) +
                    (double)(now_ts.tv_nsec - prev_net_ts.tv_nsec) / 1e9;
        if (dt <= 0) dt = 1;
        sent_rate = (double)(cur_net.sent - prev_net.sent) / dt;
        recv_rate = (double)(cur_net.recv - prev_net.recv) / dt;
    }
    prev_net = cur_net;
    prev_net_ts = now_ts;
    have_prev_net = 1;

    pthread_mutex_unlock(&g_lock);

    docker_c_t containers[MAX_DOCKER];
    int dn = get_docker_stats_cached(containers, MAX_DOCKER);

    char hostname[256] = {0};
    gethostname(hostname, sizeof(hostname));

    struct timespec rt; clock_gettime(CLOCK_REALTIME, &rt);
    long long ts_ms = (long long)rt.tv_sec * 1000 + rt.tv_nsec / 1000000;
    long long boot_ms = read_boot_time() * 1000LL;

    sbuf_appendf(s, "{\"hostname\":");
    json_escape_append(s, hostname);
    sbuf_appendf(s, ",\"ts\":%lld,\"boot_time\":%lld,", ts_ms, boot_ms);

    sbuf_appendf(s, "\"cpu\":{\"percent\":%.2f,\"cores\":[", cpu_overall);
    for (int i = 0; i < ncores; i++) sbuf_appendf(s, "%s%.2f", i ? "," : "", per_core[i]);
    sbuf_appendf(s, "],\"count\":%d},", ncores);

    sbuf_appendf(s, "\"memory\":{\"percent\":%.2f,\"used\":%lld,\"total\":%lld,\"available\":%lld},",
                 mem.percent, mem.used, mem.total, mem.available);

    sbuf_appendf(s, "\"swap\":{\"percent\":%.2f,\"used\":%lld,\"total\":%lld},",
                 swap.percent, swap.used, swap.total);

    sbuf_appendf(s, "\"network\":{\"sent_rate\":%.2f,\"recv_rate\":%.2f,\"sent_total\":%lld,\"recv_total\":%lld},",
                 sent_rate, recv_rate, cur_net.sent, cur_net.recv);

    sbuf_appendf(s, "\"docker\":[");
    for (int i = 0; i < dn; i++) {
        if (i) sbuf_appendf(s, ",");
        sbuf_appendf(s, "{\"name\":");
        json_escape_append(s, containers[i].name);
        sbuf_appendf(s, ",\"cpu_percent\":%.2f,\"mem_usage\":", containers[i].cpu_percent);
        json_escape_append(s, containers[i].mem_usage);
        sbuf_appendf(s, ",\"mem_percent\":%.2f}", containers[i].mem_percent);
    }
    sbuf_appendf(s, "]}");
}

/* ------------------------------------------------------------------ */
/* HTTP server                                                         */
/* ------------------------------------------------------------------ */

static int send_all(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static void send_404(int fd) {
    const char *body = "Not Found";
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        strlen(body));
    send_all(fd, header, (size_t)hlen);
    send_all(fd, body, strlen(body));
}

static void handle_metrics(int fd) {
    sbuf_t s; sbuf_init(&s);
    build_metrics_json(&s);
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        s.len);
    send_all(fd, header, (size_t)hlen);
    send_all(fd, s.buf, s.len);
    sbuf_free(&s);
}

/*
 * Waits up to `ms` milliseconds; returns 1 as soon as the peer is detected
 * to have disconnected (during the wait or immediately), 0 if it's still
 * connected once the wait elapses. Without this, an EventSource client that
 * vanishes without a clean TCP close (page closed mid-request, network
 * drop, a killed test client, ...) leaves this loop running forever,
 * quietly accumulating threads and — since each tick reads the cached
 * Docker stats — never actually costing much, but still worth reaping.
 */
static int wait_or_client_gone(int fd, int ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int r = poll(&pfd, 1, ms);
    if (r <= 0) return 0;
    if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) return 1;
    if (pfd.revents & POLLIN) {
        char buf[1];
        ssize_t n = recv(fd, buf, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n == 0) return 1;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return 1;
    }
    return 0;
}

static void handle_stream(int fd) {
    static const char header[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "X-Accel-Buffering: no\r\n\r\n";
    send_all(fd, header, strlen(header));

    /* warm up cpu_percent, matching the python warm-up call */
    cpu_times_t warm[MAX_CORES + 1];
    int nc;
    pthread_mutex_lock(&g_lock);
    read_cpu_times(warm, MAX_CORES + 1, &nc);
    memcpy(g_cpu_prev, warm, sizeof(cpu_times_t) * (size_t)(nc + 1 > MAX_CORES + 1 ? MAX_CORES + 1 : nc + 1));
    g_cpu_have_prev = 1;
    pthread_mutex_unlock(&g_lock);
    usleep(500000);

    for (;;) {
        sbuf_t s; sbuf_init(&s);
        build_metrics_json(&s);
        if (send(fd, "data: ", 6, MSG_NOSIGNAL) <= 0) { sbuf_free(&s); break; }
        if (send(fd, s.buf, s.len, MSG_NOSIGNAL) <= 0) { sbuf_free(&s); break; }
        if (send(fd, "\n\n", 2, MSG_NOSIGNAL) <= 0) { sbuf_free(&s); break; }
        sbuf_free(&s);
        if (wait_or_client_gone(fd, 250)) break;
    }
}

static const char *content_type_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".js")) return "application/javascript";
    if (!strcmp(dot, ".css")) return "text/css";
    if (!strcmp(dot, ".json")) return "application/json";
    if (!strcmp(dot, ".svg")) return "image/svg+xml";
    if (!strcmp(dot, ".png")) return "image/png";
    if (!strcmp(dot, ".ico")) return "image/x-icon";
    return "application/octet-stream";
}

static void handle_static(int fd, const char *reqpath) {
    if (strstr(reqpath, "..")) { send_404(fd); return; }

    char filepath[4096];
    if (!strcmp(reqpath, "/")) snprintf(filepath, sizeof(filepath), "%s/index.html", STATIC_DIR);
    else snprintf(filepath, sizeof(filepath), "%s%s", STATIC_DIR, reqpath);

    struct stat st;
    if (stat(filepath, &st) != 0 || S_ISDIR(st.st_mode)) { send_404(fd); return; }

    FILE *f = fopen(filepath, "rb");
    if (!f) { send_404(fd); return; }

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\nConnection: close\r\n\r\n",
        content_type_for(filepath), (long long)st.st_size);
    send_all(fd, header, (size_t)hlen);

    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) send_all(fd, buf, n);
    fclose(f);
}

static int read_headers(int fd, char *buf, size_t bufsz) {
    size_t total = 0;
    while (total < bufsz - 1) {
        ssize_t n = recv(fd, buf + total, bufsz - 1 - total, 0);
        if (n <= 0) return total > 0 ? (int)total : -1;
        total += (size_t)n;
        buf[total] = 0;
        if (strstr(buf, "\r\n\r\n")) return (int)total;
    }
    return (int)total;
}

static void *client_thread(void *arg) {
    int fd = (int)(intptr_t)arg;

    char buf[8192];
    int n = read_headers(fd, buf, sizeof(buf));
    if (n <= 0) { close(fd); return NULL; }

    char method[8] = {0}, path[2048] = {0};
    sscanf(buf, "%7s %2047s", method, path);
    char *q = strchr(path, '?');
    if (q) *q = 0;

    if (!strcmp(path, "/stream")) handle_stream(fd);
    else if (!strcmp(path, "/metrics")) handle_metrics(fd);
    else handle_static(fd, path);

    close(fd);
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    const char *hp = getenv("ENV_HOST_PROC");
    if (hp && *hp) { strncpy(host_proc, hp, sizeof(host_proc) - 1); host_proc[sizeof(host_proc) - 1] = 0; }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(srv, 64) < 0) { perror("listen"); return 1; }

    pthread_t docker_th;
    pthread_create(&docker_th, NULL, docker_stats_thread, NULL);
    pthread_detach(docker_th);

    printf("MachineMonitor (C) listening on :%d (host_proc=%s)\n", PORT, host_proc);
    fflush(stdout);

    /*
     * build_metrics_json() and its callees keep several MAX_CORES-sized
     * arrays (~32KB each) on the stack. glibc's default pthread stack
     * (8MB) swallows that without noticing, but musl (Alpine) defaults to
     * a stack small enough that this reliably overflowed it and crashed
     * the process with SIGSEGV on the first real request. Give client
     * threads an explicit, generous stack regardless of which libc is in
     * use.
     */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1 << 20); /* 1 MiB */

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        pthread_t th;
        pthread_create(&th, &attr, client_thread, (void *)(intptr_t)fd);
        pthread_detach(th);
    }
}
