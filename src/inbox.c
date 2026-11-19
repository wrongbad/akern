#include "inbox.h"

#include <stdlib.h>

#include "msg.h"
#include "sink.h"

typedef struct item {
    enum source source;
    enum when   when;
    size_t      seq;
    json_buf    content;
} item;

static const char *const when_names[] = { "now", "turn", "idle", "deferred" };

const char *when_name(enum when when)
{
    for (size_t i = 0; i < LEN(when_names); i++)
        if (when == 1u << i) return when_names[i];
    return "idle";
}

bool when_parse(span name_or_json_string, enum when *when)
{
    span name = name_or_json_string;
    if (json_is(name, '"')) name = (span){ name.p + 1, name.n - 2 };
    for (size_t i = 0; i < LEN(when_names); i++)
        if (span_eq(name, when_names[i])) {
            *when = (enum when)(1u << i);
            return true;
        }
    return false;
}

static const char *source_name(enum source source)
{
    return source == FROM_JOB ? "job" : source == FROM_INJECT ? "inject" : "prompt";
}

static void remove_item(inbox *in, item *it)
{
    VEC_REMOVE(in->items, in->nitems, it);
    buf_free(&it->content);
    free(it);
}

void inbox_add(inbox *in, enum source source, enum when when, json content)
{
    item *it = xcalloc(1, sizeof *it);
    it->source = source;
    it->when = when;
    it->seq = in->nadded++;
    buf_set(&it->content, content);
    VEC_PUSH(in->items, in->nitems, in->capitems, it);
    event(in->sink, "{'type':'queued','source':%s,'when':%s,'n':%d}", source_name(source), when_name(when),
          (int)in->nitems);
}

static bool due_from(const inbox *in, unsigned when_mask, size_t first_seq)
{
    for (size_t i = 0; i < in->nitems; i++)
        if (in->items[i]->seq >= first_seq && (in->items[i]->when & when_mask)) return true;
    return false;
}

bool inbox_due(const inbox *in, unsigned when_mask)            { return due_from(in, when_mask, 0); }
bool inbox_due_since_mark(const inbox *in, unsigned when_mask) { return due_from(in, when_mask, in->mark); }
void inbox_mark(inbox *in)                                     { in->mark = in->nadded; }

static bool take_one(inbox *in, unsigned when_mask, bool prompts_only, void (*put)(json_list *, json), json_list *out)
{
    for (size_t i = 0; i < in->nitems; i++) {
        item *it = in->items[i];
        if (!(it->when & when_mask) || (prompts_only && it->source == FROM_JOB)) continue;
        put(out, buf_span(&it->content));
        remove_item(in, it);
        return true;
    }
    return false;
}

size_t inbox_take(inbox *in, unsigned when_mask, json_list *blocks)
{
    size_t n = 0;
    if (when_mask) when_mask |= WHEN_DEFERRED;
    while (take_one(in, when_mask, false, blocks_add_content, blocks)) n++;
    return n;
}

static void add_content(json_list *contents, json content)
{
    if (contents->n) buf_putc(contents, ',');
    buf_putspan(contents, content);
}

void inbox_return_prompts(inbox *in, unsigned when_mask, json_list *contents)
{
    while (take_one(in, when_mask, true, add_content, contents)) {}
}

void inbox_defer_jobs(inbox *in)
{
    for (size_t i = 0; i < in->nitems; i++)
        if (in->items[i]->source == FROM_JOB) in->items[i]->when = WHEN_DEFERRED;
}

size_t inbox_take_jobs(inbox *in, json_list *blocks)
{
    size_t n = 0;
    for (size_t i = 0; i < in->nitems;)
        if (in->items[i]->source == FROM_JOB)
            blocks_add_content(blocks, buf_span(&in->items[i]->content)), remove_item(in, in->items[i]), n++;
        else i++;
    return n;
}

size_t inbox_size(const inbox *in)
{
    return in->nitems;
}
