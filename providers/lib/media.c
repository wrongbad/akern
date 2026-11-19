#include "media.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    json block, base64;
    bool image;
    const char *omit_reason;
} media;

static struct {
    media  *v;
    size_t  n, cap, base64_bytes, document_bytes;
} found;

static void record_media(json block)
{
    json type = json_get(block, "type"), source = json_get(block, "source");
    bool image = json_eq(type, "image");
    if (!(image || json_eq(type, "document")) || !json_eq(json_get(source, "type"), "base64")) return;
    json base64 = json_get(source, "data");
    media m = { .block = block, .base64 = base64, .image = image };
    VEC_PUSH(found.v, found.n, found.cap, m);
    found.base64_bytes += base64.n;
    if (!image) found.document_bytes += base64.n;
}

static bool carries_results(json block)
{
    json type = json_get(block, "type");
    return json_eq(type, "tool_result") || json_eq(type, "job_result");
}

static void record_all_media(json content)
{
    jiter it = json_iter(content);
    for (json block; json_next(&it, NULL, &block);) {
        record_media(block);
        if (carries_results(block)) record_all_media(json_get(block, "content"));
    }
}

static void omit(media *m, const char *reason)
{
    if (!m->omit_reason) m->omit_reason = reason;
}

static void drop_oldest(bool documents_only, size_t *bytes, size_t budget)
{
    for (size_t i = 0; i < found.n && budget && *bytes > budget; i++) {
        if (documents_only && found.v[i].image) continue;
        *bytes -= found.v[i].base64.n;
        omit(&found.v[i], "dropped, oldest first, to keep the request under the API's size limit");
    }
}

static void judge_media_limits(const media_limits *limits)
{
    static char too_large[64];
    snprintf(too_large, sizeof too_large, "larger than the API's %zu MB limit", limits->image_bytes >> 20);
    for (size_t i = 0; i < found.n; i++)
        if (found.v[i].image && limits->image_bytes && found.v[i].base64.n > limits->image_bytes) omit(&found.v[i], too_large);
    drop_oldest(true, &found.document_bytes, limits->document_budget_bytes);
    drop_oldest(false, &found.base64_bytes, limits->budget_bytes);
}

static const media *omitted_media_at(const char *block_start)
{
    for (size_t i = 0; i < found.n; i++)
        if (found.v[i].block.p == block_start && found.v[i].omit_reason) return &found.v[i];
    return NULL;
}

static bool contains_omitted_media(json v)
{
    for (size_t i = 0; i < found.n; i++)
        if (found.v[i].omit_reason && found.v[i].block.p >= v.p && found.v[i].block.p < v.p + v.n) return true;
    return false;
}

static void put_object_within_limits(json_buf *out, json object);

static void put_array_within_limits(json_buf *out, json array)
{
    if (!contains_omitted_media(array) || !json_is(array, '[')) {
        buf_putspan(out, array);
        return;
    }
    jiter it = json_iter(array);
    buf_putc(out, '[');
    for (json element; json_next(&it, NULL, &element);) {
        const media *m = omitted_media_at(element.p);
        json_comma_unless_first(out);
        if (m) json_printf(out, "{'type':'text','text':'[%r omitted: %r. The original is still in the session.]'}",
                           m->image ? "image" : "document", m->omit_reason);
        else put_object_within_limits(out, element);
    }
    buf_putc(out, ']');
}

static void put_object_within_limits(json_buf *out, json object)
{
    if (!contains_omitted_media(object)) {
        buf_putspan(out, object);
        return;
    }
    jiter it = json_iter(object);
    bool first = true;
    buf_putc(out, '{');
    for (json k, v; json_next(&it, &k, &v); first = false) {
        json_printf(out, "%r%R:", first ? "" : ",", k);
        if (json_eq(k, "content")) put_array_within_limits(out, v);
        else buf_putspan(out, v);
    }
    buf_putc(out, '}');
}

void media_put_within_limits(json_buf *out, json messages, const media_limits *limits)
{
    jiter it = json_iter(messages);
    for (json message; json_next(&it, NULL, &message);) record_all_media(json_get(message, "content"));
    judge_media_limits(limits);
    put_array_within_limits(out, json_trim(messages));
    free(found.v);
    memset(&found, 0, sizeof found);
}
