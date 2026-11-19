/* base: the foundation every module builds on, shared with the providers. Owns memory
   allocation (running out is fatal), byte buffers, spans, string vectors and pointer arrays,
   complete writes to a descriptor, and clocks. Knows nothing of JSON, processes or the
   conversation. */

#ifndef BASE_H
#define BASE_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

/* A char * returned (except by buf_cstr) or held in a struct is owned; a const char * is borrowed. */
typedef struct { const char *p; size_t n; } span;
typedef struct { char *p; size_t n, cap; } buf;
typedef struct { char **v; size_t n, cap; } strv;

#define NOSPAN ((span){ NULL, 0 })
#define LEN(a) (sizeof(a) / sizeof((a)[0]))

void  *xmalloc(size_t n);
void  *xcalloc(size_t n, size_t size);
void  *xrealloc(void *p, size_t n);
char  *xstrdup(const char *s);
void   die(const char *fmt, ...);
bool   write_all(int fd, span bytes);

span   S(const char *s);
span   buf_span(const buf *b);
void   buf_put(buf *b, const void *p, size_t n);
void   buf_puts(buf *b, const char *s);
void   buf_putc(buf *b, char c);
void   buf_putspan(buf *b, span s);
void   buf_printf(buf *b, const char *fmt, ...);
void   buf_set(buf *b, span s);
void   buf_drop_front(buf *b, size_t n);
char  *buf_cstr(buf *b);
void   buf_free(buf *b);

bool   span_eq(span a, const char *s);
char  *span_dup(span s);

void   strv_push(strv *v, char *s);
void   strv_free(strv *v);

#define VEC_PUSH(vec, n, cap, x) do {                                   \
        if ((n) == (cap)) {                                             \
            (cap) = (cap) ? (cap) * 2 : 8;                              \
            (vec) = xrealloc((vec), (cap) * sizeof *(vec));             \
        }                                                               \
        (vec)[(n)++] = (x);                                             \
    } while (0)

#define VEC_REMOVE(vec, n, x) do {                                      \
        for (size_t i_ = 0; i_ < (n); i_++)                             \
            if ((vec)[i_] == (x)) {                                     \
                for (; i_ + 1 < (n); i_++) (vec)[i_] = (vec)[i_ + 1];   \
                (n)--;                                                  \
                break;                                                  \
            }                                                           \
    } while (0)

double    monotonic_seconds(void);
long long unix_seconds(void);
double    earliest_deadline(double a, double b);

#endif
