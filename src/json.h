/* json: all JSON handling, shared with the providers, and the only code that takes untrusted
   bytes apart. Owns checking that text is valid JSON, finding values inside it, decoding strings
   and numbers, and writing new JSON text. A value read is a slice of the original bytes, never a
   parsed copy, so what the kernel stores or resends is byte-for-byte what it received. Knows no
   message or protocol shapes; msg and the modules that use them do. */

#ifndef JSON_H
#define JSON_H

#include "base.h"

typedef span json;      /* the text of one JSON value, still escaped; NOSPAN when absent */
typedef buf  json_buf;  /* builds the text of one JSON value */
typedef buf  json_list; /* builds comma-separated array elements or object members, without the brackets */

typedef struct { const char *at, *end; char close; } jiter;

json   json_trim(json v);
bool   json_valid(span text);
bool   json_is(json v, char first);
bool   json_null_or_absent(json v);

json   json_get(json object, const char *key);
jiter  json_iter(json container);
bool   json_next(jiter *it, json *key, json *value);

bool   json_eq(json v, const char *text);
bool   json_text(json string, buf *text);
char  *json_text_dup(json string);
double json_num(json v, double fallback);
bool   json_true(json v);

/* json_printf: ' writes "; %s C string and %S span as JSON strings or null; %R json or null; %r C string raw;
   %d int; %D long long; %f double; %b bool */
void   json_put_string(buf *out, span text);
void   json_printf(buf *out, const char *fmt, ...);
void   json_vprintf(buf *out, const char *fmt, va_list ap);

#endif
