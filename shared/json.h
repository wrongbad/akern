/* json: all JSON handling, shared with the providers, and the only code that takes untrusted
   bytes apart. Owns checking that text is valid JSON, finding values inside it, decoding strings
   and numbers, and writing new JSON text. A value read is a slice of the original bytes, never a
   parsed copy, so what the kernel stores or resends is byte-for-byte what it received. Knows no
   message or protocol shapes; message and the modules that use them do. */

#ifndef JSON_H
#define JSON_H

#include "bytes.h"

typedef span json;     /* the text of one JSON value, still escaped; NOSPAN when absent */
typedef buf json_buf;  /* builds the text of one JSON value */
typedef buf json_list; /* builds array elements or object members, without brackets */

typedef struct {
    const char *at, *end;
    char close;
} jiter;

/* The value without surrounding whitespace. */
json json_trim(json v);
/* True if the text is exactly one valid value, nested up to 256 deep. */
bool json_is_valid(span text);
/* True if the value starts with this character. */
bool json_is(json v, char first);
/* True for null or an absent value. */
bool json_is_null_or_absent(json v);

/* An object's member, or NOSPAN. */
json json_get(json object, const char *key);
/* An iterator over an object's members or an array's elements. */
jiter json_iter(json container);
/* The next member (key and value) or element; false at the end. */
bool json_next(jiter *it, json *key, json *value);

/* True for a string equal to text once decoded. */
bool json_eq(json v, const char *text);
/* Appends a string's decoded text; false if not a string. */
bool json_decode(json string, buf *text);
/* A string's decoded text, owned; NULL if not a string. */
char *json_decode_dup(json string);
/* A number, or a string holding one; else fallback. */
double json_get_num(json v, double fallback);
/* True for the literal true. */
bool json_is_true(json v);
/* The text between a string's quotes, still escaped. */
span json_get_escaped_body(json string);

/* The length of the UTF-8 character starting a non-ASCII byte at p, or 0 when it is not one. */
size_t json_utf8_len(const unsigned char *p, size_t n);
/* Appends text as a JSON string; invalid UTF-8 becomes U+FFFD. */
void json_put_string(buf *out, span text);
/* Appends a comma unless the array has just opened. */
void json_put_comma_unless_first(json_buf *array);
/* ' writes "; %s %S string or null; %R json or null; %r raw; %d %D %f %b */
void json_printf(buf *out, const char *fmt, ...);
/* json_printf with a va_list. */
void json_vprintf(buf *out, const char *fmt, va_list ap);

#endif
