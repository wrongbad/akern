#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_DEPTH = 256 };

static const char *skip_ws(const char *p, const char *e)
{
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static const char *skip_string(const char *p, const char *e)
{
    for (p++; p < e; p++) {
        if (*p == '"') return p + 1;
        if (*p == '\\') p++;
        else if ((unsigned char)*p < 0x20) return NULL;
    }
    return NULL;
}

static const char *skip_word(const char *p, const char *e, const char *word)
{
    size_t n = strlen(word);
    return (size_t)(e - p) >= n && memcmp(p, word, n) == 0 ? p + n : NULL;
}

static const char *skip_number(const char *p, const char *e)
{
    const char *start = p;
    while (p < e && strchr("+-0123456789.eE", *p)) p++;
    return p > start ? p : NULL;
}

static const char *skip_value(const char *p, const char *e, int depth);

static const char *skip_members(const char *p, const char *e, char close, int depth)
{
    p = skip_ws(p + 1, e);
    if (p < e && *p == close) return p + 1;
    for (;;) {
        if (close == '}') {
            if (p >= e || *p != '"' || !(p = skip_string(p, e))) return NULL;
            p = skip_ws(p, e);
            if (p >= e || *p != ':') return NULL;
            p++;
        }
        if (!(p = skip_value(p, e, depth + 1))) return NULL;
        p = skip_ws(p, e);
        if (p >= e) return NULL;
        if (*p == close) return p + 1;
        if (*p != ',') return NULL;
        p = skip_ws(p + 1, e);
    }
}

static const char *skip_value(const char *p, const char *e, int depth)
{
    p = skip_ws(p, e);
    if (p >= e || depth > MAX_DEPTH) return NULL;
    switch (*p) {
    case '{': return skip_members(p, e, '}', depth);
    case '[': return skip_members(p, e, ']', depth);
    case '"': return skip_string(p, e);
    case 't': return skip_word(p, e, "true");
    case 'f': return skip_word(p, e, "false");
    case 'n': return skip_word(p, e, "null");
    default:  return skip_number(p, e);
    }
}

json json_trim(json v)
{
    if (!v.p) return v;
    const char *p = skip_ws(v.p, v.p + v.n), *e = v.p + v.n;
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) e--;
    return (json){ p, (size_t)(e - p) };
}

bool json_valid(span text)
{
    json v = json_trim(text);
    return v.n && skip_value(v.p, v.p + v.n, 0) == v.p + v.n;
}

bool json_is(json v, char first)
{
    return v.p && v.n && v.p[0] == first;
}

bool json_null_or_absent(json v)
{
    return !v.p || span_eq(v, "null");
}

jiter json_iter(json container)
{
    json c = json_trim(container);
    if (!json_is(c, '{') && !json_is(c, '[')) return (jiter){ NULL, NULL, 0 };
    return (jiter){ c.p + 1, c.p + c.n, c.p[0] == '{' ? '}' : ']' };
}

bool json_next(jiter *it, json *key, json *value)
{
    if (!it->at) return false;
    const char *p = skip_ws(it->at, it->end);
    if (p < it->end && *p == ',') p = skip_ws(p + 1, it->end);
    if (p >= it->end || *p == it->close) return false;
    if (it->close == '}') {
        const char *key_start = p;
        if (!(p = skip_string(p, it->end))) return false;
        if (key) *key = (json){ key_start, (size_t)(p - key_start) };
        p = skip_ws(p, it->end);
        if (p >= it->end || *p != ':') return false;
        p = skip_ws(p + 1, it->end);
    }
    const char *value_start = p;
    if (!(p = skip_value(p, it->end, 0))) return false;
    *value = (json){ value_start, (size_t)(p - value_start) };
    it->at = p;
    return true;
}

json json_get(json object, const char *key)
{
    jiter it = json_iter(object);
    json k, v;
    if (it.close != '}') return NOSPAN;
    while (json_next(&it, &k, &v))
        if (json_eq(k, key)) return v;
    return NOSPAN;
}

static void put_utf8(buf *b, unsigned long c)
{
    if (c < 0x80) buf_putc(b, (char)c);
    else if (c < 0x800) {
        buf_putc(b, (char)(0xC0 | c >> 6));
        buf_putc(b, (char)(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        buf_putc(b, (char)(0xE0 | c >> 12));
        buf_putc(b, (char)(0x80 | (c >> 6 & 0x3F)));
        buf_putc(b, (char)(0x80 | (c & 0x3F)));
    } else {
        buf_putc(b, (char)(0xF0 | c >> 18));
        buf_putc(b, (char)(0x80 | (c >> 12 & 0x3F)));
        buf_putc(b, (char)(0x80 | (c >> 6 & 0x3F)));
        buf_putc(b, (char)(0x80 | (c & 0x3F)));
    }
}

static unsigned long parse_hex4(const char *p, const char *e)
{
    char tmp[5] = { 0 };
    if (e - p < 4) return 0xFFFD;
    memcpy(tmp, p, 4);
    return strtoul(tmp, NULL, 16);
}

bool json_text(json string, buf *text)
{
    if (!json_is(string, '"') || string.n < 2) return false;
    const char *p = string.p + 1, *e = string.p + string.n - 1;
    while (p < e) {
        if (*p != '\\') { buf_putc(text, *p++); continue; }
        if (++p >= e) break;
        char c = *p++;
        switch (c) {
        case 'n': buf_putc(text, '\n'); break;
        case 't': buf_putc(text, '\t'); break;
        case 'r': buf_putc(text, '\r'); break;
        case 'b': buf_putc(text, '\b'); break;
        case 'f': buf_putc(text, '\f'); break;
        case 'u': {
            unsigned long code = parse_hex4(p, e);
            p += 4;
            if (code >= 0xD800 && code < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                unsigned long low = parse_hex4(p + 2, e);
                if (low >= 0xDC00 && low < 0xE000) {
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    p += 6;
                }
            }
            put_utf8(text, code);
            break;
        }
        default: buf_putc(text, c);
        }
    }
    return true;
}

bool json_eq(json v, const char *text)
{
    if (!json_is(v, '"') || v.n < 2) return false;
    span escaped = { v.p + 1, v.n - 2 };
    if (!memchr(escaped.p, '\\', escaped.n)) return span_eq(escaped, text);
    buf decoded = { 0 };
    json_text(v, &decoded);
    bool eq = span_eq(buf_span(&decoded), text);
    buf_free(&decoded);
    return eq;
}

char *json_text_dup(json string)
{
    buf text = { 0 };
    if (!json_text(string, &text)) return NULL;
    return buf_cstr(&text);
}

double json_num(json v, double fallback)
{
    if (!v.p || !v.n || !strchr("-0123456789\"", v.p[0])) return fallback;
    char tmp[64] = { 0 };
    span digits = json_is(v, '"') ? (span){ v.p + 1, v.n - 2 } : v;
    if (digits.n >= sizeof tmp) return fallback;
    memcpy(tmp, digits.p, digits.n);
    char *end;
    double d = strtod(tmp, &end);
    return end == tmp ? fallback : d;
}

bool json_true(json v)
{
    return span_eq(v, "true");
}

static size_t utf8_len(const unsigned char *p, size_t n)
{
    size_t len = p[0] >= 0xF0 && p[0] <= 0xF4 ? 4 : p[0] >= 0xE0 ? 3 : p[0] >= 0xC2 && p[0] < 0xE0 ? 2 : 0;
    if (!len || len > n) return 0;
    for (size_t i = 1; i < len; i++)
        if ((p[i] & 0xC0) != 0x80) return 0;
    return len;
}

void json_put_string(buf *b, span text)
{
    const unsigned char *p = (const unsigned char *)text.p, *e = p + text.n;
    buf_putc(b, '"');
    while (p < e) {
        unsigned char c = *p;
        if (c >= 0x80) {
            size_t len = utf8_len(p, (size_t)(e - p));
            if (len) buf_put(b, p, len), p += len;
            else buf_puts(b, "\\ufffd"), p++;
            continue;
        }
        switch (c) {
        case '"':  buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        case '\n': buf_puts(b, "\\n"); break;
        case '\t': buf_puts(b, "\\t"); break;
        case '\r': buf_puts(b, "\\r"); break;
        default:
            if (c < 0x20) buf_printf(b, "\\u%04x", c);
            else buf_putc(b, (char)c);
        }
        p++;
    }
    buf_putc(b, '"');
}

void json_vprintf(buf *b, const char *f, va_list ap)
{
    for (; *f; f++) {
        if (*f == '\'') { buf_putc(b, '"'); continue; }
        if (*f != '%') { buf_putc(b, *f); continue; }
        switch (*++f) {
        case 'd': buf_printf(b, "%d", va_arg(ap, int)); break;
        case 'D': buf_printf(b, "%lld", va_arg(ap, long long)); break;
        case 'f': buf_printf(b, "%.15g", va_arg(ap, double)); break;
        case 'b': buf_puts(b, va_arg(ap, int) ? "true" : "false"); break;
        case 'r': buf_puts(b, va_arg(ap, const char *)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s) json_put_string(b, S(s));
            else buf_puts(b, "null");
            break;
        }
        case 'S': {
            span s = va_arg(ap, span);
            if (s.p) json_put_string(b, s);
            else buf_puts(b, "null");
            break;
        }
        case 'R': {
            json v = json_trim(va_arg(ap, json));
            if (v.n) buf_putspan(b, v);
            else buf_puts(b, "null");
            break;
        }
        default: buf_putc(b, *f);
        }
    }
}

void json_printf(buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    json_vprintf(b, fmt, ap);
    va_end(ap);
}
