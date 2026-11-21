/* bytes: holding and growing bytes, shared with the providers. Owns memory allocation
   (running out is fatal), byte buffers, spans, string vectors and pointer arrays, and
   complete writes to a descriptor. Knows nothing of JSON, processes, time or the
   conversation. */

#ifndef BYTES_H
#define BYTES_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

/* A char * returned (except by buf_cstr) or held in a struct is owned;
   a const char * is borrowed. */
typedef struct {
    const char *p;
    size_t n;
} span;
typedef struct {
    char *p;
    size_t n, cap;
} buf;
typedef struct {
    char **v;
    size_t n, cap;
} strv;

#define NOSPAN ((span){NULL, 0})
#define LEN(a) (sizeof(a) / sizeof((a)[0]))

/* malloc; running out of memory is fatal. */
void *xmalloc(size_t n);
/* calloc; running out of memory is fatal. */
void *xcalloc(size_t n, size_t size);
/* realloc; running out of memory is fatal. */
void *xrealloc(void *p, size_t n);
/* strdup; running out of memory is fatal. */
char *xstrdup(const char *s);
/* Prints akern: MESSAGE on stderr and exits 2. */
void die(const char *fmt, ...);
/* Writes every byte, retrying on EINTR; false on error. */
bool write_all(int fd, span bytes);

/* A span over a C string. */
span S(const char *s);
/* The buffer's bytes as a span. */
span buf_span(const buf *b);
/* Appends n bytes. */
void buf_put(buf *b, const void *p, size_t n);
/* Appends a C string. */
void buf_puts(buf *b, const char *s);
/* Appends one byte. */
void buf_putc(buf *b, char c);
/* Appends a span's bytes. */
void buf_putspan(buf *b, span s);
/* Appends printf-formatted text. */
void buf_printf(buf *b, const char *fmt, ...);
/* Replaces the contents with a span's bytes. */
void buf_set(buf *b, span s);
/* Removes the first n bytes. */
void buf_drop_front(buf *b, size_t n);
/* The contents as a C string, terminated in place; not owned. */
char *buf_cstr(buf *b);
/* Frees the bytes and empties the buffer. */
void buf_free(buf *b);

/* True if the span holds exactly the C string. */
bool span_eq(span a, const char *s);
/* An owned, NUL-terminated copy of the span. */
char *span_dup(span s);

/* Appends an owned string. */
void strv_push(strv *v, char *s);
/* Frees every string and the vector. */
void strv_free(strv *v);

/* Appends x, doubling the capacity as needed. */
#define VEC_PUSH(vec, n, cap, x)                                                                   \
    do                                                                                             \
    {                                                                                              \
        if ((n) == (cap))                                                                          \
        {                                                                                          \
            (cap) = (cap) ? (cap) * 2 : 8;                                                         \
            (vec) = xrealloc((vec), (cap) * sizeof *(vec));                                        \
        }                                                                                          \
        (vec)[(n)++] = (x);                                                                        \
    } while (0)

/* Removes the first element equal to x, keeping the order. */
#define VEC_REMOVE(vec, n, x)                                                                      \
    do                                                                                             \
    {                                                                                              \
        for (size_t i_ = 0; i_ < (n); i_++)                                                        \
            if ((vec)[i_] == (x))                                                                  \
            {                                                                                      \
                for (; i_ + 1 < (n); i_++)                                                         \
                    (vec)[i_] = (vec)[i_ + 1];                                                     \
                (n)--;                                                                             \
                break;                                                                             \
            }                                                                                      \
    } while (0)

#endif
