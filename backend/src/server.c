/*
 * server.c - SyncLab backend entry point.
 *
 * A dependency-free HTTP + WebSocket server (POSIX sockets + pthreads) that exposes
 * exactly the REST / WebSocket API the React frontend already speaks:
 *
 *   GET  /api/health
 *   POST /api/simulation/{create,start,pause,resume,stop,reset,compare}
 *   GET  /api/simulation/{state,events,events/export,results}
 *   WS   /ws/events?last_seq=N
 *   GET  (anything else) -> static files from frontend/dist
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "sync.h"
#include "util.h"

#define MAX_HEADER_BYTES (16 * 1024)
#define MAX_BODY_BYTES   (1024 * 1024)
#define MAX_WS_CLIENTS   256

/* ======================= request / response ======================= */

typedef struct {
    char   method[16];
    char   path[2048];
    char   query[2048];
    char   origin[256];
    char   acr_method[64];
    char   acr_headers[512];
    char   ws_key[128];
    char   upgrade[32];
    long   content_length;
    char  *body;
    size_t body_len;
} Request;

static int send_all(int fd, const char *data, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, data + off, n - off, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static const char *reason(int status) {
    switch (status) {
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 422: return "Unprocessable Entity";
        default:  return "Internal Server Error";
    }
}

static void respond(int fd, const Request *rq, int status, const char *ctype, const char *body, size_t len,
                    const char *extra, int head_only) {
    StrBuf h;
    sb_init(&h);
    sb_printf(&h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n",
              status, reason(status), ctype, len);
    if (rq && rq->origin[0])
        sb_printf(&h, "Access-Control-Allow-Origin: %s\r\nAccess-Control-Allow-Credentials: true\r\nVary: Origin\r\n", rq->origin);
    if (extra) sb_puts(&h, extra);
    sb_puts(&h, "\r\n");
    send_all(fd, h.data, h.len);
    if (!head_only && len) send_all(fd, body, len);
    sb_free(&h);
}

static void respond_json_str(int fd, const Request *rq, int status, const char *json) {
    respond(fd, rq, status, "application/json", json, strlen(json), NULL, 0);
}
static void respond_json(int fd, const Request *rq, int status, StrBuf *sb) {
    respond(fd, rq, status, "application/json", sb->data, sb->len, NULL, 0);
}
static void respond_detail(int fd, const Request *rq, int status, const char *msg) {
    StrBuf sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"detail\":");
    sb_json_str(&sb, msg);
    sb_putc(&sb, '}');
    respond_json(fd, rq, status, &sb);
    sb_free(&sb);
}

static int hdr_is(const char *line, const char *name) {
    size_t n = strlen(name);
    return strncasecmp(line, name, n) == 0 && line[n] == ':';
}
static void hdr_copy(char *dst, size_t cap, const char *line) {
    const char *v = strchr(line, ':') + 1;
    while (*v == ' ' || *v == '\t') v++;
    snprintf(dst, cap, "%s", v);
}

static void url_decode(char *dst, size_t cap, const char *src, int plus_is_space) {
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < cap; i++) {
        if (src[i] == '%' && isxdigit((unsigned char)src[i + 1]) && isxdigit((unsigned char)src[i + 2])) {
            char hx[3] = { src[i + 1], src[i + 2], 0 };
            dst[o++] = (char)strtol(hx, NULL, 16);
            i += 2;
        } else if (plus_is_space && src[i] == '+') {
            dst[o++] = ' ';
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

/* returns 1 if key present */
static int query_get(const char *query, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    const char *p = query;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t seglen = amp ? (size_t)(amp - p) : strlen(p);
        if (seglen >= kl && strncmp(p, key, kl) == 0 && (seglen == kl || p[kl] == '=')) {
            char tmp[512];
            const char *v = (seglen > kl) ? p + kl + 1 : "";
            size_t vl = (seglen > kl) ? seglen - kl - 1 : 0;
            if (vl >= sizeof tmp) vl = sizeof tmp - 1;
            memcpy(tmp, v, vl);
            tmp[vl] = '\0';
            url_decode(out, cap, tmp, 1);
            return 1;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return 0;
}

static int read_request(int fd, Request *rq) {
    memset(rq, 0, sizeof *rq);
    char *buf = (char *)malloc(MAX_HEADER_BYTES + 1);
    if (!buf) return -1;
    size_t have = 0;
    char *hdr_end = NULL;
    while (!hdr_end) {
        if (have >= MAX_HEADER_BYTES) { free(buf); return -1; }
        ssize_t r = recv(fd, buf + have, MAX_HEADER_BYTES - have, 0);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; free(buf); return -1; }
        have += (size_t)r;
        buf[have] = '\0';
        hdr_end = strstr(buf, "\r\n\r\n");
    }
    size_t header_len = (size_t)(hdr_end - buf) + 4;
    *hdr_end = '\0';

    /* request line */
    char *line = buf;
    char *eol = strstr(line, "\r\n");
    if (eol) *eol = '\0';
    char target[2048];
    if (sscanf(line, "%15s %2047s", rq->method, target) != 2) { free(buf); return -1; }
    char *q = strchr(target, '?');
    if (q) { *q = '\0'; snprintf(rq->query, sizeof rq->query, "%s", q + 1); }
    snprintf(rq->path, sizeof rq->path, "%s", target);

    /* headers */
    line = eol ? eol + 2 : (char *)"";
    while (*line) {
        eol = strstr(line, "\r\n");
        if (eol) *eol = '\0';
        if (hdr_is(line, "Content-Length")) rq->content_length = atol(strchr(line, ':') + 1);
        else if (hdr_is(line, "Origin")) hdr_copy(rq->origin, sizeof rq->origin, line);
        else if (hdr_is(line, "Access-Control-Request-Method")) hdr_copy(rq->acr_method, sizeof rq->acr_method, line);
        else if (hdr_is(line, "Access-Control-Request-Headers")) hdr_copy(rq->acr_headers, sizeof rq->acr_headers, line);
        else if (hdr_is(line, "Sec-WebSocket-Key")) hdr_copy(rq->ws_key, sizeof rq->ws_key, line);
        else if (hdr_is(line, "Upgrade")) hdr_copy(rq->upgrade, sizeof rq->upgrade, line);
        if (!eol) break;
        line = eol + 2;
    }

    if (rq->content_length < 0 || rq->content_length > MAX_BODY_BYTES) { free(buf); return -2; }
    if (rq->content_length > 0) {
        rq->body = (char *)malloc((size_t)rq->content_length + 1);
        if (!rq->body) { free(buf); return -1; }
        size_t already = have > header_len ? have - header_len : 0;
        if (already > (size_t)rq->content_length) already = (size_t)rq->content_length;
        memcpy(rq->body, buf + header_len, already);
        size_t got = already;
        while (got < (size_t)rq->content_length) {
            ssize_t r = recv(fd, rq->body + got, (size_t)rq->content_length - got, 0);
            if (r <= 0) { if (r < 0 && errno == EINTR) continue; free(buf); return -1; }
            got += (size_t)r;
        }
        rq->body[got] = '\0';
        rq->body_len = got;
    }
    free(buf);
    return 0;
}

/* ======================= validation (FastAPI-style 422) ======================= */

typedef struct {
    StrBuf errs;
    int    n;
} ErrList;

static void err_add(ErrList *l, const char *where, const char *field, const char *type, const char *msg) {
    if (l->n++) sb_putc(&l->errs, ',');
    sb_printf(&l->errs, "{\"type\":\"%s\",\"loc\":[\"%s\",\"%s\"],\"msg\":", type, where, field);
    sb_json_str(&l->errs, msg);
    sb_putc(&l->errs, '}');
}

static void respond_422(int fd, const Request *rq, ErrList *l) {
    StrBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "{\"detail\":[%s]}", l->errs.data);
    respond_json(fd, rq, 422, &sb);
    sb_free(&sb);
}

/* Validates one integer field. Returns 1 if value assigned (or default kept) without error. */
static void int_field(const JObject *o, const char *name, int *dst, int minv, int maxv, ErrList *errs) {
    const JField *f = jobj_get(o, name);
    if (!f) return; /* default kept */
    long long v = 0;
    if (f->type == JV_NUM) {
        if (f->num != (double)(long long)f->num) {
            err_add(errs, "body", name, "int_from_float", "Input should be a valid integer, got a number with a fractional part");
            return;
        }
        v = (long long)f->num;
    } else if (f->type == JV_STR) {
        char *end;
        errno = 0;
        v = strtoll(f->str, &end, 10);
        if (f->str[0] == '\0' || *end != '\0' || errno) {
            err_add(errs, "body", name, "int_parsing", "Input should be a valid integer, unable to parse string as an integer");
            return;
        }
    } else {
        err_add(errs, "body", name, "int_type", "Input should be a valid integer");
        return;
    }
    char msg[96];
    if (v < minv) {
        snprintf(msg, sizeof msg, "Input should be greater than or equal to %d", minv);
        err_add(errs, "body", name, "greater_than_equal", msg);
        return;
    }
    if (v > maxv) {
        snprintf(msg, sizeof msg, "Input should be less than or equal to %d", maxv);
        err_add(errs, "body", name, "less_than_equal", msg);
        return;
    }
    *dst = (int)v;
}

/* returns 0 ok; on failure writes the 422 response and returns -1 */
static int parse_config(int fd, const Request *rq, SimConfig *cfg) {
    simconfig_default(cfg);
    ErrList errs;
    sb_init(&errs.errs);
    errs.n = 0;

    if (!rq->body || rq->body_len == 0) {
        sb_puts(&errs.errs, "{\"type\":\"missing\",\"loc\":[\"body\"],\"msg\":\"Field required\",\"input\":null}");
        errs.n = 1;
        respond_422(fd, rq, &errs);
        sb_free(&errs.errs);
        return -1;
    }
    JObject o;
    int pr = json_parse_object(rq->body, rq->body_len, &o);
    if (pr == 1) {
        sb_puts(&errs.errs, "{\"type\":\"json_invalid\",\"loc\":[\"body\",0],\"msg\":\"JSON decode error\",\"ctx\":{\"error\":\"Expecting value\"}}");
        errs.n = 1;
        respond_422(fd, rq, &errs);
        sb_free(&errs.errs);
        return -1;
    }
    if (pr == 2) {
        sb_puts(&errs.errs, "{\"type\":\"model_attributes_type\",\"loc\":[\"body\"],\"msg\":\"Input should be a valid dictionary or object to extract fields from\"}");
        errs.n = 1;
        respond_422(fd, rq, &errs);
        sb_free(&errs.errs);
        return -1;
    }

    const JField *f = jobj_get(&o, "algorithm");
    if (f) {
        if (f->type != JV_STR || !algo_parse(f->str, &cfg->algorithm))
            err_add(&errs, "body", "algorithm", "enum", "Input should be 'reader_preference', 'writer_preference' or 'fair_fifo'");
    }
    f = jobj_get(&o, "writer_op");
    if (f) {
        if (f->type != JV_STR || !wop_parse(f->str, &cfg->writer_op))
            err_add(&errs, "body", "writer_op", "enum", "Input should be 'ADD', 'SUBTRACT', 'MULTIPLY' or 'SET'");
    }
    int_field(&o, "reader_count", &cfg->reader_count, 1, MAX_READERS, &errs);
    int_field(&o, "writer_count", &cfg->writer_count, 1, MAX_WRITERS, &errs);
    int_field(&o, "read_duration_ms", &cfg->read_duration_ms, 100, 10000, &errs);
    int_field(&o, "write_duration_ms", &cfg->write_duration_ms, 100, 10000, &errs);
    int_field(&o, "initial_value", &cfg->initial_value, -10000, 10000, &errs);
    int_field(&o, "writer_amount", &cfg->writer_amount, 1, 1000, &errs);
    int_field(&o, "max_duration_sec", &cfg->max_duration_sec, 5, 300, &errs);
    int_field(&o, "stagger_arrivals_ms", &cfg->stagger_arrivals_ms, 0, 2000, &errs);

    if (errs.n) {
        respond_422(fd, rq, &errs);
        sb_free(&errs.errs);
        return -1;
    }
    sb_free(&errs.errs);
    return 0;
}

/* ======================= WebSocket plumbing ======================= */

typedef struct {
    int fd;
    pthread_mutex_t send_mu;
} WsConn;

static WsConn *g_ws[MAX_WS_CLIENTS];
static int g_nws = 0;
static pthread_mutex_t g_ws_mu = PTHREAD_MUTEX_INITIALIZER;

static int ws_send(WsConn *c, int opcode, const char *data, size_t len) {
    uint8_t hdr[10];
    size_t hl = 0;
    hdr[hl++] = (uint8_t)(0x80 | (opcode & 0x0f));
    if (len < 126) {
        hdr[hl++] = (uint8_t)len;
    } else if (len <= 0xFFFF) {
        hdr[hl++] = 126;
        hdr[hl++] = (uint8_t)(len >> 8);
        hdr[hl++] = (uint8_t)len;
    } else {
        hdr[hl++] = 127;
        for (int i = 7; i >= 0; i--) hdr[hl++] = (uint8_t)((uint64_t)len >> (8 * i));
    }
    pthread_mutex_lock(&c->send_mu);
    int rc = send_all(c->fd, (const char *)hdr, hl);
    if (rc == 0 && len) rc = send_all(c->fd, data, len);
    pthread_mutex_unlock(&c->send_mu);
    return rc;
}

/* Called from engine worker threads for every event. */
static void ws_broadcast(const char *json, size_t len, void *ctx) {
    (void)ctx;
    pthread_mutex_lock(&g_ws_mu);
    for (int i = 0; i < g_nws; i++) {
        if (ws_send(g_ws[i], 1, json, len) != 0) shutdown(g_ws[i]->fd, SHUT_RDWR); /* its handler thread cleans up */
    }
    pthread_mutex_unlock(&g_ws_mu);
}

static int ws_count(void) {
    pthread_mutex_lock(&g_ws_mu);
    int n = g_nws;
    pthread_mutex_unlock(&g_ws_mu);
    return n;
}

static int recv_all(int fd, uint8_t *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, buf + got, n - got, 0);
        if (r == 0) return -1;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        got += (size_t)r;
    }
    return 0;
}

/* Reads one complete message (reassembling fragments). Returns opcode, or -1 on close/error. */
static int ws_read_message(int fd, StrBuf *payload) {
    int msg_op = -1;
    for (;;) {
        uint8_t h[2];
        if (recv_all(fd, h, 2) != 0) return -1;
        int fin = h[0] & 0x80, op = h[0] & 0x0f, masked = h[1] & 0x80;
        uint64_t len = h[1] & 0x7f;
        if (len == 126) {
            uint8_t e[2];
            if (recv_all(fd, e, 2) != 0) return -1;
            len = ((uint64_t)e[0] << 8) | e[1];
        } else if (len == 127) {
            uint8_t e[8];
            if (recv_all(fd, e, 8) != 0) return -1;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
        }
        if (len > (uint64_t)MAX_BODY_BYTES) return -1;
        uint8_t mask[4] = { 0, 0, 0, 0 };
        if (masked && recv_all(fd, mask, 4) != 0) return -1;
        uint8_t *tmp = (uint8_t *)malloc(len ? (size_t)len : 1);
        if (!tmp) return -1;
        if (len && recv_all(fd, tmp, (size_t)len) != 0) { free(tmp); return -1; }
        if (masked) for (uint64_t i = 0; i < len; i++) tmp[i] ^= mask[i & 3];

        if (op >= 8) { /* control frame: hand it straight back */
            sb_reset(payload);
            sb_putn(payload, (const char *)tmp, (size_t)len);
            free(tmp);
            return op;
        }
        if (op != 0) { msg_op = op; sb_reset(payload); }
        sb_putn(payload, (const char *)tmp, (size_t)len);
        free(tmp);
        if (fin) return msg_op < 0 ? 1 : msg_op;
    }
}

/* ======================= engine ownership ======================= */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER; /* guards g_engine */
static Engine *g_engine = NULL;

static Engine *engine_locked(void) {
    if (!g_engine) {
        SimConfig c;
        simconfig_default(&c);
        g_engine = engine_create(&c, ws_broadcast, NULL);
    }
    return g_engine;
}

static void ws_handle(int fd, const Request *rq) {
    if (strcasecmp(rq->upgrade, "websocket") != 0 || !rq->ws_key[0]) {
        respond_detail(fd, rq, 400, "Expected a WebSocket upgrade request");
        return;
    }
    static const char *GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char cat[256];
    snprintf(cat, sizeof cat, "%s%s", rq->ws_key, GUID);
    uint8_t dig[20];
    sha1((const uint8_t *)cat, strlen(cat), dig);
    char acc[64];
    base64_encode(dig, 20, acc);

    char resp[256];
    int rl = snprintf(resp, sizeof resp,
                      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: %s\r\n\r\n", acc);
    if (send_all(fd, resp, (size_t)rl) != 0) return;

    struct timeval none = { 0, 0 }, snd = { 3, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof snd);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);

    char tmp[32];
    int last_seq = 0;
    if (query_get(rq->query, "last_seq", tmp, sizeof tmp)) {
        char *end;
        long v = strtol(tmp, &end, 10);
        if (*tmp && *end == '\0' && v >= 0 && v < INT_MAX) last_seq = (int)v;
    }

    WsConn *c = (WsConn *)calloc(1, sizeof *c);
    if (!c) return;
    c->fd = fd;
    pthread_mutex_init(&c->send_mu, NULL);

    /* register first so no live event is missed, then send snapshot + replay (the UI de-duplicates by seq) */
    pthread_mutex_lock(&g_ws_mu);
    if (g_nws >= MAX_WS_CLIENTS) {
        pthread_mutex_unlock(&g_ws_mu);
        ws_send(c, 8, "\x03\xf0", 2);
        pthread_mutex_destroy(&c->send_mu);
        free(c);
        return;
    }
    g_ws[g_nws++] = c;
    pthread_mutex_unlock(&g_ws_mu);

    StrBuf snap, missed;
    sb_init(&snap);
    sb_init(&missed);
    pthread_mutex_lock(&g_lock);
    Engine *e = engine_locked();
    sb_puts(&snap, "{\"type\":\"SNAPSHOT\",\"state\":");
    engine_state_json(e, &snap);
    sb_putc(&snap, '}');
    engine_events_after(e, last_seq, &missed);
    pthread_mutex_unlock(&g_lock);

    int ok = (ws_send(c, 1, snap.data, snap.len) == 0);
    if (ok) {
        char *p = missed.data;
        while (ok && p && *p) {
            char *nl = strchr(p, '\n');
            if (!nl) break;
            ok = (ws_send(c, 1, p, (size_t)(nl - p)) == 0);
            p = nl + 1;
        }
    }
    sb_free(&snap);
    sb_free(&missed);

    StrBuf payload;
    sb_init(&payload);
    while (ok) {
        int op = ws_read_message(fd, &payload);
        if (op < 0) break;
        if (op == 8) { ws_send(c, 8, payload.data, payload.len >= 2 ? 2 : 0); break; }
        if (op == 9) { ws_send(c, 10, payload.data, payload.len); continue; }
        if (op == 10) continue;
        if (op == 1) {
            JObject o;
            if (json_parse_object(payload.data, payload.len, &o) == 0) {
                const JField *t = jobj_get(&o, "type");
                if (t && t->type == JV_STR && strcmp(t->str, "PING") == 0) {
                    char pong[96];
                    int pl = snprintf(pong, sizeof pong, "{\"type\":\"PONG\",\"timestamp\":%.6f}", wall_now());
                    if (ws_send(c, 1, pong, (size_t)pl) != 0) break;
                }
            }
        }
    }
    sb_free(&payload);

    pthread_mutex_lock(&g_ws_mu);
    for (int i = 0; i < g_nws; i++) {
        if (g_ws[i] == c) { g_ws[i] = g_ws[--g_nws]; break; }
    }
    pthread_mutex_unlock(&g_ws_mu);
    pthread_mutex_destroy(&c->send_mu);
    free(c);
}

/* ======================= API handlers ======================= */

static void respond_state(int fd, const Request *rq, Engine *e) {
    StrBuf sb;
    sb_init(&sb);
    engine_state_json(e, &sb);
    respond_json(fd, rq, 200, &sb);
    sb_free(&sb);
}

static int is_method(const Request *rq, const char *m) { return strcmp(rq->method, m) == 0; }

static void method_not_allowed(int fd, const Request *rq, const char *allow) {
    char hdr[64];
    snprintf(hdr, sizeof hdr, "Allow: %s\r\n", allow);
    StrBuf sb;
    sb_init(&sb);
    sb_puts(&sb, "{\"detail\":\"Method Not Allowed\"}");
    respond(fd, rq, 405, "application/json", sb.data, sb.len, hdr, 0);
    sb_free(&sb);
}

static void handle_compare(int fd, const Request *rq, SimConfig cfg) {
    static const Algo algos[3] = { ALGO_READER_PREF, ALGO_WRITER_PREF, ALGO_FAIR_FIFO };
    StrBuf out;
    sb_init(&out);
    sb_puts(&out, "{\"workload\":");
    simconfig_json(&out, &cfg);
    sb_puts(&out, ",\"comparison\":{");
    for (int i = 0; i < 3; i++) {
        SimConfig c = cfg;
        c.algorithm = algos[i];
        Engine *t = engine_create(&c, NULL, NULL);
        if (!t) continue;
        engine_start(t);
        double t0 = mono_now();
        while (engine_status(t) == SS_RUNNING && (mono_now() - t0) < (double)c.max_duration_sec) usleep(50 * 1000);
        engine_stop(t);   /* no-op if already completed */
        engine_join(t);
        Metrics m;
        int64_t val;
        int ver, tot;
        engine_metrics(t, &m);
        engine_summary(t, &val, &ver, &tot);
        if (i) sb_putc(&out, ',');
        sb_printf(&out, "\"%s\":{\"algorithm\":\"%s\",\"metrics\":", algo_name(c.algorithm), algo_name(c.algorithm));
        engine_metrics_json(&out, &m);
        sb_printf(&out, ",\"final_value\":%lld,\"final_version\":%d,\"total_events\":%d}", (long long)val, ver, tot);
        engine_destroy(t);
    }
    sb_puts(&out, "},\"summary\":\"Comparison benchmark completed using identical concurrency workload.\"}");
    respond_json(fd, rq, 200, &out);
    sb_free(&out);
}

static void handle_api(int fd, const Request *rq) {
    const char *p = rq->path;

    if (strcmp(p, "/api/health") == 0) {
        if (!is_method(rq, "GET")) return method_not_allowed(fd, rq, "GET");
        char buf[256];
        snprintf(buf, sizeof buf,
                 "{\"status\":\"ok\",\"service\":\"SyncLab Simulator Backend\",\"timestamp\":%.6f,\"active_websockets\":%d}",
                 wall_now(), ws_count());
        return respond_json_str(fd, rq, 200, buf);
    }

    if (strcmp(p, "/api/simulation/create") == 0) {
        if (!is_method(rq, "POST")) return method_not_allowed(fd, rq, "POST");
        SimConfig cfg;
        if (parse_config(fd, rq, &cfg) != 0) return;
        pthread_mutex_lock(&g_lock);
        if (g_engine) {
            g_engine->broadcast = 0;      /* the old run is being replaced: don't leak its stop events to the UI */
            engine_destroy(g_engine);
        }
        g_engine = engine_create(&cfg, ws_broadcast, NULL);
        respond_state(fd, rq, g_engine);
        pthread_mutex_unlock(&g_lock);
        return;
    }

    static const char *actions[] = { "start", "pause", "resume", "stop", "reset" };
    for (int i = 0; i < 5; i++) {
        char route[64];
        snprintf(route, sizeof route, "/api/simulation/%s", actions[i]);
        if (strcmp(p, route) != 0) continue;
        if (!is_method(rq, "POST")) return method_not_allowed(fd, rq, "POST");
        pthread_mutex_lock(&g_lock);
        Engine *e = engine_locked();
        switch (i) {
            case 0: {
                SimStatus s = engine_status(e);
                if (s == SS_COMPLETED || s == SS_CANCELLED) engine_reset(e);
                engine_start(e);
                break;
            }
            case 1: engine_pause(e); break;
            case 2: engine_resume(e); break;
            case 3: engine_stop(e); break;
            case 4: engine_reset(e); break;
        }
        respond_state(fd, rq, e);
        pthread_mutex_unlock(&g_lock);
        return;
    }

    if (strcmp(p, "/api/simulation/state") == 0) {
        if (!is_method(rq, "GET")) return method_not_allowed(fd, rq, "GET");
        pthread_mutex_lock(&g_lock);
        respond_state(fd, rq, engine_locked());
        pthread_mutex_unlock(&g_lock);
        return;
    }

    if (strcmp(p, "/api/simulation/events") == 0) {
        if (!is_method(rq, "GET")) return method_not_allowed(fd, rq, "GET");
        ErrList errs;
        sb_init(&errs.errs);
        errs.n = 0;
        int min_seq = 0, limit = 500, ptype = -1;
        char val[128], etype[64] = "";
        int have_etype = 0;
        if (query_get(rq->query, "min_seq", val, sizeof val)) {
            char *end;
            long v = strtol(val, &end, 10);
            if (!*val || *end) err_add(&errs, "query", "min_seq", "int_parsing", "Input should be a valid integer, unable to parse string as an integer");
            else if (v < 0) err_add(&errs, "query", "min_seq", "greater_than_equal", "Input should be greater than or equal to 0");
            else min_seq = v > INT_MAX ? INT_MAX : (int)v;
        }
        if (query_get(rq->query, "limit", val, sizeof val)) {
            char *end;
            long v = strtol(val, &end, 10);
            if (!*val || *end) err_add(&errs, "query", "limit", "int_parsing", "Input should be a valid integer, unable to parse string as an integer");
            else if (v < 1) err_add(&errs, "query", "limit", "greater_than_equal", "Input should be greater than or equal to 1");
            else if (v > 2000) err_add(&errs, "query", "limit", "less_than_equal", "Input should be less than or equal to 2000");
            else limit = (int)v;
        }
        if (query_get(rq->query, "event_type", etype, sizeof etype)) have_etype = 1;
        if (query_get(rq->query, "process_type", val, sizeof val)) {
            if (strcmp(val, "READER") == 0) ptype = PT_READER;
            else if (strcmp(val, "WRITER") == 0) ptype = PT_WRITER;
            else err_add(&errs, "query", "process_type", "enum", "Input should be 'READER' or 'WRITER'");
        }
        if (errs.n) { respond_422(fd, rq, &errs); sb_free(&errs.errs); return; }
        sb_free(&errs.errs);

        StrBuf sb;
        sb_init(&sb);
        pthread_mutex_lock(&g_lock);
        engine_events_json(engine_locked(), &sb, min_seq, have_etype ? etype : NULL, ptype, limit);
        pthread_mutex_unlock(&g_lock);
        respond_json(fd, rq, 200, &sb);
        sb_free(&sb);
        return;
    }

    if (strcmp(p, "/api/simulation/events/export") == 0) {
        if (!is_method(rq, "GET")) return method_not_allowed(fd, rq, "GET");
        char fmt[32] = "csv";
        query_get(rq->query, "format", fmt, sizeof fmt);
        if (strcmp(fmt, "csv") != 0 && strcmp(fmt, "json") != 0) {
            ErrList errs;
            sb_init(&errs.errs);
            errs.n = 0;
            err_add(&errs, "query", "format", "string_pattern_mismatch", "String should match pattern '^(csv|json)$'");
            respond_422(fd, rq, &errs);
            sb_free(&errs.errs);
            return;
        }
        StrBuf sb;
        sb_init(&sb);
        char sim_id[16], hdr[160];
        pthread_mutex_lock(&g_lock);
        Engine *e = engine_locked();
        snprintf(sim_id, sizeof sim_id, "%s", e->simulation_id);
        if (strcmp(fmt, "json") == 0) engine_events_json(e, &sb, 0, NULL, -1, INT_MAX);
        else engine_events_csv(e, &sb);
        pthread_mutex_unlock(&g_lock);
        snprintf(hdr, sizeof hdr, "Content-Disposition: attachment; filename=synclab-events-%s.%s\r\n", sim_id, fmt);
        respond(fd, rq, 200, strcmp(fmt, "json") == 0 ? "application/json" : "text/csv; charset=utf-8", sb.data, sb.len, hdr, 0);
        sb_free(&sb);
        return;
    }

    if (strcmp(p, "/api/simulation/results") == 0) {
        if (!is_method(rq, "GET")) return method_not_allowed(fd, rq, "GET");
        StrBuf sb;
        sb_init(&sb);
        pthread_mutex_lock(&g_lock);
        engine_results_json(engine_locked(), &sb);
        pthread_mutex_unlock(&g_lock);
        respond_json(fd, rq, 200, &sb);
        sb_free(&sb);
        return;
    }

    if (strcmp(p, "/api/simulation/compare") == 0) {
        if (!is_method(rq, "POST")) return method_not_allowed(fd, rq, "POST");
        SimConfig cfg;
        if (parse_config(fd, rq, &cfg) != 0) return;
        return handle_compare(fd, rq, cfg);
    }

    respond_detail(fd, rq, 404, "Not Found");
}

/* ======================= static files ======================= */

static char g_dist[PATH_MAX] = "";      /* resolved frontend/dist (empty = API only) */

static const char *mime_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    struct { const char *ext, *mime; } t[] = {
        { ".html", "text/html; charset=utf-8" }, { ".js", "text/javascript; charset=utf-8" },
        { ".mjs", "text/javascript; charset=utf-8" }, { ".css", "text/css; charset=utf-8" },
        { ".json", "application/json" }, { ".map", "application/json" }, { ".svg", "image/svg+xml" },
        { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".gif", "image/gif" },
        { ".ico", "image/x-icon" }, { ".webp", "image/webp" }, { ".woff2", "font/woff2" }, { ".woff", "font/woff" },
        { ".ttf", "font/ttf" }, { ".txt", "text/plain; charset=utf-8" }, { ".webmanifest", "application/manifest+json" },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (strcasecmp(dot, t[i].ext) == 0) return t[i].mime;
    return "application/octet-stream";
}

static void handle_static(int fd, const Request *rq) {
    int head = is_method(rq, "HEAD");
    if (!is_method(rq, "GET") && !head) return method_not_allowed(fd, rq, "GET, HEAD");
    if (!g_dist[0]) return respond_detail(fd, rq, 404, "Not Found");

    char decoded[2048];
    url_decode(decoded, sizeof decoded, rq->path, 0);
    if (strstr(decoded, "..") || strchr(decoded, '\\')) return respond_detail(fd, rq, 404, "Not Found");

    char full[PATH_MAX + 2100];
    snprintf(full, sizeof full, "%s%s", g_dist, decoded);
    struct stat st;
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
        size_t l = strlen(full);
        snprintf(full + l, sizeof full - l, "%sindex.html", (l && full[l - 1] == '/') ? "" : "/");
    }
    char real[PATH_MAX];
    if (!realpath(full, real) || strncmp(real, g_dist, strlen(g_dist)) != 0 ||
        stat(real, &st) != 0 || !S_ISREG(st.st_mode))
        return respond_detail(fd, rq, 404, "Not Found");

    FILE *f = fopen(real, "rb");
    if (!f) return respond_detail(fd, rq, 404, "Not Found");
    char *data = (char *)malloc((size_t)st.st_size + 1);
    size_t n = data ? fread(data, 1, (size_t)st.st_size, f) : 0;
    fclose(f);
    if (!data) return respond_detail(fd, rq, 500, "Out of memory");
    respond(fd, rq, 200, mime_for(real), data, n, "Cache-Control: no-cache\r\n", head);
    free(data);
}

/* ======================= connection threads / main ======================= */

static void handle_connection(int fd) {
    Request rq;
    int rc = read_request(fd, &rq);
    if (rc == -2) { respond_detail(fd, NULL, 413, "Request body too large"); return; }
    if (rc != 0) return;

    if (strcmp(rq.method, "OPTIONS") == 0 && rq.acr_method[0]) {
        StrBuf h;
        sb_init(&h);
        sb_printf(&h, "Access-Control-Allow-Methods: DELETE, GET, HEAD, OPTIONS, PATCH, POST, PUT\r\nAccess-Control-Max-Age: 600\r\n");
        if (rq.acr_headers[0]) sb_printf(&h, "Access-Control-Allow-Headers: %s\r\n", rq.acr_headers);
        respond(fd, &rq, 200, "text/plain; charset=utf-8", "OK", 2, h.data, 0);
        sb_free(&h);
    } else if (strcmp(rq.path, "/ws/events") == 0) {
        ws_handle(fd, &rq);
    } else if (strncmp(rq.path, "/api/", 5) == 0) {
        handle_api(fd, &rq);
    } else {
        handle_static(fd, &rq);
    }
    free(rq.body);
}

static void *conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    handle_connection(fd);
    shutdown(fd, SHUT_RDWR);
    close(fd);
    return NULL;
}

static int dist_ok(const char *path, char *out) {
    char idx[PATH_MAX + 16];
    snprintf(idx, sizeof idx, "%s/index.html", path);
    struct stat st;
    if (stat(idx, &st) != 0) return 0;
    return realpath(path, out) != NULL;
}

static void find_dist(const char *override) {
    if (override) {
        if (!dist_ok(override, g_dist)) fprintf(stderr, "warning: no index.html in --dist '%s'\n", override);
        return;
    }
    const char *env = getenv("SYNCLAB_DIST");
    if (env && dist_ok(env, g_dist)) return;
    if (dist_ok("../frontend/dist", g_dist)) return;   /* run from synclab/backend */
    if (dist_ok("frontend/dist", g_dist)) return;      /* run from synclab/ */
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) *slash = '\0';
        char cand[PATH_MAX + 32];
        const char *rel[] = { "/../../frontend/dist", "/../frontend/dist" };
        for (int i = 0; i < 2; i++) {
            snprintf(cand, sizeof cand, "%s%s", exe, rel[i]);
            if (dist_ok(cand, g_dist)) return;
        }
    }
}

static void usage(const char *argv0) {
    printf("Usage: %s [--host ADDR] [--port N] [--dist DIR]\n"
           "  --host   bind address            (default 127.0.0.1, env SYNCLAB_HOST)\n"
           "  --port   TCP port                (default 8000,      env SYNCLAB_PORT)\n"
           "  --dist   built frontend directory (default ../frontend/dist, env SYNCLAB_DIST)\n", argv0);
}

int main(int argc, char **argv) {
    const char *host = getenv("SYNCLAB_HOST") ? getenv("SYNCLAB_HOST") : "127.0.0.1";
    int port = getenv("SYNCLAB_PORT") ? atoi(getenv("SYNCLAB_PORT")) : 8000;
    const char *dist = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dist") && i + 1 < argc) dist = argv[++i];
        else { usage(argv[0]); return !strcmp(argv[i], "--help") || !strcmp(argv[i], "-h") ? 0 : 1; }
    }
    if (port <= 0 || port > 65535) { fprintf(stderr, "invalid port\n"); return 1; }

    signal(SIGPIPE, SIG_IGN);
    find_dist(dist);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) { fprintf(stderr, "invalid host '%s'\n", host); return 1; }
    if (bind(ls, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(ls, 128) < 0) { perror("listen"); return 1; }

    printf("SyncLab backend (C, pthreads + POSIX semaphores) listening on http://%s:%d\n", host, port);
    if (g_dist[0]) printf("Serving frontend from %s\n", g_dist);
    else printf("Frontend dist not found - API only (run the Vite dev server for the UI)\n");
    fflush(stdout);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 512 * 1024);

    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            usleep(100 * 1000);
            continue;
        }
        struct timeval rcv = { 15, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof rcv);
        pthread_t t;
        if (pthread_create(&t, &attr, conn_thread, (void *)(intptr_t)fd) != 0) close(fd);
    }
}
