#define _GNU_SOURCE
#include "util.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ======================= StrBuf ======================= */

void sb_init(StrBuf *sb) {
    sb->cap = 256;
    sb->len = 0;
    sb->data = (char *)malloc(sb->cap);
    if (sb->data) sb->data[0] = '\0';
}

void sb_free(StrBuf *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

void sb_reset(StrBuf *sb) {
    sb->len = 0;
    if (sb->data) sb->data[0] = '\0';
}

static int sb_reserve(StrBuf *sb, size_t extra) {
    if (!sb->data) return 0;
    if (sb->len + extra + 1 <= sb->cap) return 1;
    size_t ncap = sb->cap;
    while (sb->len + extra + 1 > ncap) ncap *= 2;
    char *nd = (char *)realloc(sb->data, ncap);
    if (!nd) return 0;
    sb->data = nd;
    sb->cap = ncap;
    return 1;
}

void sb_putn(StrBuf *sb, const char *s, size_t n) {
    if (!sb_reserve(sb, n)) return;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_puts(StrBuf *sb, const char *s) { sb_putn(sb, s, strlen(s)); }
void sb_putc(StrBuf *sb, char c) { sb_putn(sb, &c, 1); }

void sb_printf(StrBuf *sb, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need > 0 && sb_reserve(sb, (size_t)need)) {
        vsnprintf(sb->data + sb->len, (size_t)need + 1, fmt, ap2);
        sb->len += (size_t)need;
    }
    va_end(ap2);
}

void sb_json_str(StrBuf *sb, const char *s) {
    sb_putc(sb, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"':  sb_puts(sb, "\\\""); break;
            case '\\': sb_puts(sb, "\\\\"); break;
            case '\n': sb_puts(sb, "\\n"); break;
            case '\r': sb_puts(sb, "\\r"); break;
            case '\t': sb_puts(sb, "\\t"); break;
            case '\b': sb_puts(sb, "\\b"); break;
            case '\f': sb_puts(sb, "\\f"); break;
            default:
                if (*p < 0x20) sb_printf(sb, "\\u%04x", *p);
                else sb_putc(sb, (char)*p);
        }
    }
    sb_putc(sb, '"');
}

void sb_json_dbl(StrBuf *sb, double v, int decimals) {
    char buf[64];
    fmt_py_float(buf, sizeof buf, v, decimals);
    sb_puts(sb, buf);
}

void sb_csv_field(StrBuf *sb, const char *s) {
    int quote = 0;
    for (const char *p = s; *p; p++)
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') { quote = 1; break; }
    if (!quote) { sb_puts(sb, s); return; }
    sb_putc(sb, '"');
    for (const char *p = s; *p; p++) {
        if (*p == '"') sb_putc(sb, '"');
        sb_putc(sb, *p);
    }
    sb_putc(sb, '"');
}

/* ======================= time ======================= */

double mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

double wall_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void iso_now(char *buf, size_t n) {
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
#ifdef _WIN32
    gmtime_s(&tm, &ts.tv_sec);
#else
    gmtime_r(&ts.tv_sec, &tm);
#endif
    snprintf(buf, n, "%04d-%02d-%02dT%02d:%02d:%02d.%06ld+00:00",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000);
}

double round_to(double v, int decimals) {
    double m = pow(10.0, decimals);
    return floor(v * m + 0.5) / m;
}

void fmt_py_float(char *buf, size_t n, double v, int decimals) {
    if (!isfinite(v)) v = 0.0;
    snprintf(buf, n, "%.*f", decimals, v);
    char *dot = strchr(buf, '.');
    if (!dot) return;
    char *end = buf + strlen(buf) - 1;
    while (end > dot + 1 && *end == '0') *end-- = '\0';
}

/* ======================= JSON object reader ======================= */

typedef struct {
    const char *p;
    const char *end;
} JCur;

static void j_ws(JCur *c) {
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) c->p++;
}

static int hexv(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static void utf8_put(char *dst, size_t cap, size_t *len, unsigned cp) {
    char tmp[4];
    size_t k;
    if (cp < 0x80) { tmp[0] = (char)cp; k = 1; }
    else if (cp < 0x800) { tmp[0] = (char)(0xC0 | (cp >> 6)); tmp[1] = (char)(0x80 | (cp & 0x3F)); k = 2; }
    else if (cp < 0x10000) { tmp[0] = (char)(0xE0 | (cp >> 12)); tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[2] = (char)(0x80 | (cp & 0x3F)); k = 3; }
    else { tmp[0] = (char)(0xF0 | (cp >> 18)); tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (cp & 0x3F)); k = 4; }
    if (*len + k < cap) { memcpy(dst + *len, tmp, k); *len += k; }
}

/* Parse a JSON string at cursor (which points at the opening quote). Copies up to cap-1 bytes. */
static int j_string(JCur *c, char *dst, size_t cap) {
    size_t len = 0;
    if (c->p >= c->end || *c->p != '"') return 0;
    c->p++;
    while (c->p < c->end) {
        unsigned char ch = (unsigned char)*c->p++;
        if (ch == '"') { if (dst) dst[len] = '\0'; return 1; }
        if (ch < 0x20) return 0;
        if (ch == '\\') {
            if (c->p >= c->end) return 0;
            char e = *c->p++;
            unsigned cp = 0;
            switch (e) {
                case '"': cp = '"'; break;
                case '\\': cp = '\\'; break;
                case '/': cp = '/'; break;
                case 'b': cp = '\b'; break;
                case 'f': cp = '\f'; break;
                case 'n': cp = '\n'; break;
                case 'r': cp = '\r'; break;
                case 't': cp = '\t'; break;
                case 'u': {
                    if (c->end - c->p < 4) return 0;
                    for (int i = 0; i < 4; i++) {
                        int h = hexv(c->p[i]);
                        if (h < 0) return 0;
                        cp = (cp << 4) | (unsigned)h;
                    }
                    c->p += 4;
                    break;
                }
                default: return 0;
            }
            if (dst) utf8_put(dst, cap, &len, cp);
        } else if (dst && len + 1 < cap) {
            dst[len++] = (char)ch;
        }
    }
    return 0;
}

static int j_skip_value(JCur *c, int depth);

static int j_number(JCur *c, double *out) {
    const char *s = c->p;
    char *endp;
    if (c->p < c->end && *c->p == '-') c->p++;
    if (c->p >= c->end || !isdigit((unsigned char)*c->p)) return 0;
    while (c->p < c->end && isdigit((unsigned char)*c->p)) c->p++;
    if (c->p < c->end && *c->p == '.') {
        c->p++;
        if (c->p >= c->end || !isdigit((unsigned char)*c->p)) return 0;
        while (c->p < c->end && isdigit((unsigned char)*c->p)) c->p++;
    }
    if (c->p < c->end && (*c->p == 'e' || *c->p == 'E')) {
        c->p++;
        if (c->p < c->end && (*c->p == '+' || *c->p == '-')) c->p++;
        if (c->p >= c->end || !isdigit((unsigned char)*c->p)) return 0;
        while (c->p < c->end && isdigit((unsigned char)*c->p)) c->p++;
    }
    char tmp[64];
    size_t n = (size_t)(c->p - s);
    if (n >= sizeof tmp) n = sizeof tmp - 1;
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    *out = strtod(tmp, &endp);
    return 1;
}

static int j_lit(JCur *c, const char *lit) {
    size_t n = strlen(lit);
    if ((size_t)(c->end - c->p) < n || strncmp(c->p, lit, n) != 0) return 0;
    c->p += n;
    return 1;
}

static int j_skip_value(JCur *c, int depth) {
    if (depth > 32) return 0;
    j_ws(c);
    if (c->p >= c->end) return 0;
    char ch = *c->p;
    double d;
    if (ch == '"') return j_string(c, NULL, 0);
    if (ch == '{') {
        c->p++;
        j_ws(c);
        if (c->p < c->end && *c->p == '}') { c->p++; return 1; }
        for (;;) {
            j_ws(c);
            if (!j_string(c, NULL, 0)) return 0;
            j_ws(c);
            if (c->p >= c->end || *c->p != ':') return 0;
            c->p++;
            if (!j_skip_value(c, depth + 1)) return 0;
            j_ws(c);
            if (c->p >= c->end) return 0;
            if (*c->p == ',') { c->p++; continue; }
            if (*c->p == '}') { c->p++; return 1; }
            return 0;
        }
    }
    if (ch == '[') {
        c->p++;
        j_ws(c);
        if (c->p < c->end && *c->p == ']') { c->p++; return 1; }
        for (;;) {
            if (!j_skip_value(c, depth + 1)) return 0;
            j_ws(c);
            if (c->p >= c->end) return 0;
            if (*c->p == ',') { c->p++; continue; }
            if (*c->p == ']') { c->p++; return 1; }
            return 0;
        }
    }
    if (ch == 't') return j_lit(c, "true");
    if (ch == 'f') return j_lit(c, "false");
    if (ch == 'n') return j_lit(c, "null");
    return j_number(c, &d);
}

int json_parse_object(const char *s, size_t len, JObject *out) {
    JCur c = { s, s + len };
    memset(out, 0, sizeof *out);
    j_ws(&c);
    if (c.p >= c.end) return 1;
    if (*c.p != '{') {
        /* valid JSON but not an object? */
        JCur probe = c;
        if (!j_skip_value(&probe, 0)) return 1;
        j_ws(&probe);
        return probe.p == probe.end ? 2 : 1;
    }
    c.p++;
    j_ws(&c);
    if (c.p < c.end && *c.p == '}') {
        c.p++;
    } else {
        for (;;) {
            JField tmp;
            memset(&tmp, 0, sizeof tmp);
            j_ws(&c);
            if (!j_string(&c, tmp.key, sizeof tmp.key)) return 1;
            j_ws(&c);
            if (c.p >= c.end || *c.p != ':') return 1;
            c.p++;
            j_ws(&c);
            if (c.p >= c.end) return 1;
            char ch = *c.p;
            if (ch == '"') {
                tmp.type = JV_STR;
                if (!j_string(&c, tmp.str, sizeof tmp.str)) return 1;
            } else if (ch == 't') {
                tmp.type = JV_BOOL; tmp.b = 1;
                if (!j_lit(&c, "true")) return 1;
            } else if (ch == 'f') {
                tmp.type = JV_BOOL; tmp.b = 0;
                if (!j_lit(&c, "false")) return 1;
            } else if (ch == 'n') {
                tmp.type = JV_NULL;
                if (!j_lit(&c, "null")) return 1;
            } else if (ch == '{' || ch == '[') {
                tmp.type = JV_OTHER;
                if (!j_skip_value(&c, 0)) return 1;
            } else {
                tmp.type = JV_NUM;
                if (!j_number(&c, &tmp.num)) return 1;
            }
            /* later duplicates overwrite earlier ones, like Python's json */
            int replaced = 0;
            for (int i = 0; i < out->n; i++) {
                if (strcmp(out->f[i].key, tmp.key) == 0) { out->f[i] = tmp; replaced = 1; break; }
            }
            if (!replaced && out->n < JOBJ_MAX_FIELDS) out->f[out->n++] = tmp;
            j_ws(&c);
            if (c.p >= c.end) return 1;
            if (*c.p == ',') { c.p++; continue; }
            if (*c.p == '}') { c.p++; break; }
            return 1;
        }
    }
    j_ws(&c);
    return c.p == c.end ? 0 : 1;
}

const JField *jobj_get(const JObject *o, const char *key) {
    for (int i = 0; i < o->n; i++)
        if (strcmp(o->f[i].key, key) == 0) return &o->f[i];
    return NULL;
}

/* ======================= SHA-1 / Base64 ======================= */

static uint32_t rol32(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

void sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t h0 = 0x67452301u, h1 = 0xEFCDAB89u, h2 = 0x98BADCFEu, h3 = 0x10325476u, h4 = 0xC3D2E1F0u;
    size_t newlen = ((len + 8) / 64 + 1) * 64;
    uint8_t *msg = (uint8_t *)calloc(newlen, 1);
    if (!msg) { memset(out, 0, 20); return; }
    memcpy(msg, data, len);
    msg[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8u;
    for (int i = 0; i < 8; i++) msg[newlen - 1 - (size_t)i] = (uint8_t)(bits >> (8 * i));

    for (size_t off = 0; off < newlen; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)msg[off + 4 * (size_t)i] << 24) | ((uint32_t)msg[off + 4 * (size_t)i + 1] << 16) |
                   ((uint32_t)msg[off + 4 * (size_t)i + 2] << 8) | (uint32_t)msg[off + 4 * (size_t)i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
            uint32_t t = rol32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol32(b, 30); b = a; a = t;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    free(msg);
    uint32_t hs[5] = { h0, h1, h2, h3, h4 };
    for (int i = 0; i < 5; i++) {
        out[4 * i]     = (uint8_t)(hs[i] >> 24);
        out[4 * i + 1] = (uint8_t)(hs[i] >> 16);
        out[4 * i + 2] = (uint8_t)(hs[i] >> 8);
        out[4 * i + 3] = (uint8_t)hs[i];
    }
}

void base64_encode(const uint8_t *in, size_t len, char *out) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= (uint32_t)in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? tbl[v & 63] : '=';
    }
    out[o] = '\0';
}
