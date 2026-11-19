#include "base.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p) die("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

char *xstrdup(const char *s)
{
    return span_dup(S(s));
}

void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("akern: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

span S(const char *s)
{
    return s ? (span){ s, strlen(s) } : NOSPAN;
}

span buf_span(const buf *b)
{
    return (span){ b->p, b->n };
}

static void reserve(buf *b, size_t extra)
{
    if (b->n + extra + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->n + extra + 1) cap *= 2;
    b->p = xrealloc(b->p, cap);
    b->cap = cap;
}

void buf_put(buf *b, const void *p, size_t n)
{
    reserve(b, n);
    if (n) memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = 0;
}

void buf_puts(buf *b, const char *s)  { buf_put(b, s, strlen(s)); }
void buf_putc(buf *b, char c)         { buf_put(b, &c, 1); }
void buf_putspan(buf *b, span s)      { buf_put(b, s.p, s.n); }

void buf_printf(buf *b, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    reserve(b, (size_t)n);
    vsnprintf(b->p + b->n, (size_t)n + 1, fmt, ap2);
    b->n += (size_t)n;
    va_end(ap2);
    va_end(ap);
}

void buf_set(buf *b, span s)
{
    b->n = 0;
    buf_putspan(b, s);
}

void buf_drop_front(buf *b, size_t n)
{
    if (n >= b->n) n = b->n;
    memmove(b->p, b->p + n, b->n - n);
    b->n -= n;
    if (b->p) b->p[b->n] = 0;
}

char *buf_cstr(buf *b)
{
    reserve(b, 0);
    b->p[b->n] = 0;
    return b->p;
}

void buf_free(buf *b)
{
    free(b->p);
    *b = (buf){ 0 };
}

bool span_eq(span a, const char *s)
{
    size_t n = strlen(s);
    return a.p && a.n == n && memcmp(a.p, s, n) == 0;
}

char *span_dup(span s)
{
    char *p = xmalloc(s.n + 1);
    if (s.n) memcpy(p, s.p, s.n);
    p[s.n] = 0;
    return p;
}

void strv_push(strv *v, char *s)
{
    VEC_PUSH(v->v, v->n, v->cap, s);
}

void strv_free(strv *v)
{
    for (size_t i = 0; i < v->n; i++) free(v->v[i]);
    free(v->v);
    *v = (strv){ 0 };
}

double monotonic_seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

long long unix_seconds(void)
{
    return (long long)time(NULL);
}

double earliest_deadline(double a, double b)
{
    if (!a) return b;
    if (!b) return a;
    return a < b ? a : b;
}

bool write_all(int fd, span bytes)
{
    for (size_t done = 0; done < bytes.n;) {
        ssize_t w = write(fd, bytes.p + done, bytes.n - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        done += (size_t)w;
    }
    return true;
}
