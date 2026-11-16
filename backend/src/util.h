/*
 * util.h - small helpers shared by the SyncLab C backend:
 *   - growable string buffer (used to build JSON / CSV responses)
 *   - tiny JSON object reader (for request bodies)
 *   - time helpers, SHA-1 and Base64 (for the WebSocket handshake)
 */
#ifndef SYNCLAB_UTIL_H
#define SYNCLAB_UTIL_H

#include <stddef.h>
#include <stdint.h>

/* ---------- growable string buffer ---------- */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} StrBuf;

void sb_init(StrBuf *sb);
void sb_free(StrBuf *sb);
void sb_reset(StrBuf *sb);
void sb_putc(StrBuf *sb, char c);
void sb_puts(StrBuf *sb, const char *s);
void sb_putn(StrBuf *sb, const char *s, size_t n);
void sb_printf(StrBuf *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_json_str(StrBuf *sb, const char *s);          /* "quoted \"escaped\"" */
void sb_json_dbl(StrBuf *sb, double v, int decimals); /* 1200.0, 0.12, ...    */
void sb_csv_field(StrBuf *sb, const char *s);         /* RFC 4180 quoting     */

/* ---------- time ---------- */
double mono_now(void);                       /* CLOCK_MONOTONIC seconds */
double wall_now(void);                       /* CLOCK_REALTIME seconds  */
void   iso_now(char *buf, size_t n);         /* 2026-01-01T00:00:00.123456+00:00 */
double round_to(double v, int decimals);

/* Format like Python's repr() of a rounded float: 200.0, 0.12, 15.5 */
void fmt_py_float(char *buf, size_t n, double v, int decimals);

/* ---------- minimal JSON object reader ---------- */
typedef enum { JV_NULL, JV_BOOL, JV_NUM, JV_STR, JV_OTHER } JType;

typedef struct {
    char  key[48];
    JType type;
    double num;
    int    b;
    char   str[128];
} JField;

#define JOBJ_MAX_FIELDS 48
typedef struct {
    JField f[JOBJ_MAX_FIELDS];
    int    n;
} JObject;

/* Parses a flat-ish JSON object. Nested values are validated and skipped.
 * Returns 0 on success; otherwise 1 (invalid JSON) or 2 (valid JSON, not an object). */
int json_parse_object(const char *s, size_t len, JObject *out);
const JField *jobj_get(const JObject *o, const char *key);

/* ---------- hashing / encoding (WebSocket handshake) ---------- */
void sha1(const uint8_t *data, size_t len, uint8_t out[20]);
void base64_encode(const uint8_t *in, size_t len, char *out); /* out must hold 4*ceil(len/3)+1 */

#endif
