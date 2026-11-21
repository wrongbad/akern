#include "anthropic_messages.h"
#include "media.h"
#include "provider.h"

#include <stdlib.h>
#include <string.h>

static const media_limits LIMITS = {.image_bytes = 10 << 20, .budget_bytes = 100 << 20};

typedef struct {
    long rendered[3];
    block_at canonical;
} origin;

typedef enum {
    BLOCK_TEXT,
    BLOCK_THINKING,
    BLOCK_REDACTED,
    BLOCK_TOOL_USE,
    BLOCK_FALLBACK,
    BLOCK_OTHER
} block_kind;

typedef struct {
    block_kind kind;
    long stream_index;
    json_buf start_json, input_json;
    buf escaped_text, escaped_signature;
    bool dropped;
} block;

enum { USAGE_INPUT, USAGE_OUTPUT, USAGE_CACHE_READ, USAGE_CACHE_WRITE, NUSAGE };

static const char *const USAGE_KEYS[NUSAGE] = {
    "input_tokens",
    "output_tokens",
    "cache_read_input_tokens",
    "cache_creation_input_tokens",
};

static struct {
    origin *origins;
    size_t norigins, caporigins;
    long rendered_message, rendered_block;
    const char *tool_prefix;
    buf escaped_stop_reason;
    bool saw_message_stop;
    block *blocks;
    size_t nblocks, capblocks;
    long long usage_tokens[NUSAGE];
} st = {.tool_prefix = ""};

void anthropic_name_tools(const char *prefix)
{
    st.tool_prefix = prefix;
}

static bool is_extension(json name)
{
    span body = json_get_escaped_body(name);
    return body.n >= 2 && !memcmp(body.p, "x-", 2);
}

static bool has_extension_field(json object)
{
    jiter it = json_iter(object);
    for (json k, v; json_next(&it, &k, &v);)
        if (is_extension(k))
            return true;
    return false;
}

static bool is_named_document(json block)
{
    return json_eq(json_get(block, "type"), "document") && json_get(block, "name").p;
}

static bool has_named_document(json content)
{
    jiter it = json_iter(content);
    for (json block; json_next(&it, NULL, &block);)
        if (is_named_document(block))
            return true;
    return false;
}

static void put_plain(json_buf *out, json block);

static void put_fields(json_buf *out, json object, const char *dropped)
{
    jiter it = json_iter(object);
    bool first = true, document = json_eq(json_get(object, "type"), "document");
    buf_putc(out, '{');
    for (json k, v; json_next(&it, &k, &v);)
    {
        if (is_extension(k) || (dropped && json_eq(k, dropped)))
            continue;
        if (document && json_eq(k, "name"))
            json_printf(out, "%r\"title\":%R", first ? "" : ",", v);
        else if (json_eq(k, "content") && json_is(v, '[') && has_named_document(v))
        {
            json_printf(out, "%r%R:[", first ? "" : ",", k);
            jiter blocks = json_iter(v);
            for (json block; json_next(&blocks, NULL, &block);)
                json_put_comma_unless_first(out), put_plain(out, block);
            buf_putc(out, ']');
        }
        else
            json_printf(out, "%r%R:%R", first ? "" : ",", k, v);
        first = false;
    }
    buf_putc(out, '}');
}

static void put_plain(json_buf *out, json block)
{
    if (has_extension_field(block) || is_named_document(block))
        put_fields(out, block, NULL);
    else
        buf_putspan(out, block);
}

static void note_media(json block, long rendered_inner, block_at canonical)
{
    json type = json_get(block, "type");
    if (!json_eq(type, "image") && !json_eq(type, "document"))
        return;
    origin o = {{st.rendered_message, st.rendered_block, rendered_inner}, canonical};
    VEC_PUSH(st.origins, st.norigins, st.caporigins, o);
}

static void put_pending(json_buf *out, json pending)
{
    buf text = {0};
    provider_describe_pending(&text, pending);
    json_printf(
        out,
        "{'type':'tool_result','tool_use_id':%R,'content':[{'type':'text','text':%S}]}",
        json_get(pending, "tool_use_id"),
        buf_span(&text)
    );
    buf_free(&text);
}

static void put_job_result(json_buf *out, json result, long message, long index)
{
    buf text = {0};
    provider_describe_job(&text, result);
    json_put_comma_unless_first(out);
    json_printf(out, "{'type':'text','text':%S}", buf_span(&text));
    st.rendered_block++;
    jiter it = json_iter(json_get(result, "content"));
    long inner = 0;
    for (json block; json_next(&it, NULL, &block); inner++)
    {
        json type = json_get(block, "type");
        if (json_eq(type, "text") || is_extension(type))
            continue;
        json_put_comma_unless_first(out);
        note_media(block, -1, (block_at){message, index, inner});
        put_plain(out, block);
        st.rendered_block++;
    }
    buf_free(&text);
}

static void put_tool_result(json_buf *out, json result, long message, long index)
{
    jiter it = json_iter(json_get(result, "content"));
    long inner = 0;
    for (json block; json_next(&it, NULL, &block); inner++)
        note_media(block, inner, (block_at){message, index, inner});
    put_fields(out, result, "name");
}

static bool answers_call(json block)
{
    json type = json_get(block, "type");
    return json_eq(type, "tool_pending") || json_eq(type, "tool_result");
}

static void put_user_block(json_buf *out, json block, long message, long index)
{
    json type = json_get(block, "type");
    if (is_extension(type))
        return;
    if (json_eq(type, "job_result"))
    {
        put_job_result(out, block, message, index);
        return;
    }
    json_put_comma_unless_first(out);
    if (json_eq(type, "tool_pending"))
        put_pending(out, block);
    else if (json_eq(type, "tool_result"))
        put_tool_result(out, block, message, index);
    else
        note_media(block, -1, (block_at){message, index, -1}), put_plain(out, block);
    st.rendered_block++;
}

static bool is_user(json message)
{
    return json_eq(json_get(message, "role"), "user");
}

typedef enum { OTHER_BLOCKS, CALL_ANSWERS } user_block_group;

static void put_user_blocks(json_buf *out, jiter messages, long message, user_block_group group)
{
    bool answers = group == CALL_ANSWERS;
    for (json m; json_next(&messages, NULL, &m) && is_user(m); message++)
    {
        json content = json_get(m, "content");
        if (json_is(content, '"') && !answers)
        {
            json_put_comma_unless_first(out),
                json_printf(out, "{'type':'text','text':%R}", content);
            st.rendered_block++;
        }
        jiter it = json_iter(content);
        long index = 0;
        for (json block; json_next(&it, NULL, &block); index++)
            if (answers_call(block) == answers)
                put_user_block(out, block, message, index);
    }
}

static void put_tool_use(json_buf *out, json tool_use)
{
    jiter it = json_iter(tool_use);
    bool first = true;
    buf_putc(out, '{');
    for (json k, v; json_next(&it, &k, &v);)
    {
        if (is_extension(k))
            continue;
        span name = json_get_escaped_body(v);
        json_printf(out, "%r%R:", first ? "" : ",", k);
        if (json_eq(k, "name") && *st.tool_prefix)
            buf_printf(out, "\"%s%.*s\"", st.tool_prefix, (int)name.n, name.p);
        else if (json_eq(k, "input") && !json_is(v, '{'))
            buf_puts(out, "{}");
        else
            buf_putspan(out, v);
        first = false;
    }
    buf_putc(out, '}');
}

static void put_assistant(json_buf *out, json message)
{
    json content = json_get(message, "content");
    if (!json_is(content, '['))
    {
        buf_putspan(out, message);
        return;
    }
    buf_puts(out, "{\"role\":\"assistant\",\"content\":[");
    jiter it = json_iter(content);
    for (json block; json_next(&it, NULL, &block);)
    {
        json type = json_get(block, "type");
        if (is_extension(type))
            continue;
        json_put_comma_unless_first(out);
        if (json_eq(type, "tool_use"))
            put_tool_use(out, block);
        else
            put_plain(out, block);
    }
    buf_puts(out, "]}");
}

static void put_rendered_messages(json_buf *out, json messages)
{
    jiter it = json_iter(messages), at = it;
    bool merging = false;
    buf_putc(out, '[');
    for (long index = 0; json_next(&it, NULL, &messages); at = it, index++)
    {
        bool user = is_user(messages);
        if (user && merging)
            continue;
        merging = user;
        st.rendered_block = 0;
        json_put_comma_unless_first(out);
        if (user)
        {
            buf_puts(out, "{\"role\":\"user\",\"content\":[");
            put_user_blocks(out, at, index, CALL_ANSWERS);
            put_user_blocks(out, at, index, OTHER_BLOCKS);
            buf_puts(out, "]}");
        }
        else
        {
            put_assistant(out, messages);
        }
        st.rendered_message++;
    }
    buf_putc(out, ']');
}

void anthropic_render(json_buf *out, json messages)
{
    json_buf rendered = {0};
    st.norigins = 0;
    st.rendered_message = 0;
    put_rendered_messages(&rendered, messages);
    media_put_within_limits(out, buf_span(&rendered), &LIMITS);
    buf_free(&rendered);
}

static long path_index(const char **p)
{
    char *end;
    if (**p < '0' || **p > '9')
        return -1;
    long n = strtol(*p, &end, 10);
    *p = end;
    return n;
}

bool anthropic_blame(const char *api_message, block_at *at, const char **reason)
{
    const char *colon = strstr(api_message, ": ");
    long rendered[3] = {-1, -1, -1};
    int n = 0;
    for (const char *p = api_message; colon && p < colon && n < 3; p += *p == '.' || *p == ':')
    {
        size_t len = strcspn(p, ".:");
        bool named =
            n == 0 ? len == 8 && !memcmp(p, "messages", 8) : len == 7 && !memcmp(p, "content", 7);
        p += len;
        if (named && *p == '.' && (p++, (rendered[n] = path_index(&p)) >= 0))
            n++;
    }
    for (size_t i = 0; n >= 2 && i < st.norigins; i++)
        if (!memcmp(st.origins[i].rendered, rendered, sizeof rendered))
        {
            *at = st.origins[i].canonical;
            *reason = colon + 2;
            return true;
        }
    return false;
}

static void read_usage(json usage)
{
    for (size_t i = 0; i < NUSAGE; i++)
    {
        json v = json_get(usage, USAGE_KEYS[i]);
        if (v.p && !json_is_null_or_absent(v))
            st.usage_tokens[i] = (long long)json_get_num(v, 0);
    }
}

static void start_message(json message)
{
    for (size_t i = 0; i < st.nblocks; i++)
    {
        buf_free(&st.blocks[i].start_json);
        buf_free(&st.blocks[i].escaped_text);
        buf_free(&st.blocks[i].escaped_signature);
        buf_free(&st.blocks[i].input_json);
    }
    st.nblocks = 0;
    st.escaped_stop_reason.n = 0;
    memset(st.usage_tokens, 0, sizeof st.usage_tokens);
    st.saw_message_stop = false;
    read_usage(json_get(message, "usage"));
}

static block_kind kind_of(json type)
{
    if (json_eq(type, "text"))
        return BLOCK_TEXT;
    if (json_eq(type, "thinking"))
        return BLOCK_THINKING;
    if (json_eq(type, "redacted_thinking"))
        return BLOCK_REDACTED;
    if (json_eq(type, "tool_use"))
        return BLOCK_TOOL_USE;
    if (json_eq(type, "fallback"))
        return BLOCK_FALLBACK;
    return BLOCK_OTHER;
}

static block *block_for(json event)
{
    long stream_index = (long)json_get_num(json_get(event, "index"), -1);
    for (size_t i = st.nblocks; i-- > 0;)
        if (st.blocks[i].stream_index == stream_index)
            return &st.blocks[i];
    return NULL;
}

static void drop_blocks_before_fallback(void)
{
    for (size_t i = 0; i < st.nblocks; i++)
        if (st.blocks[i].kind != BLOCK_TEXT && st.blocks[i].kind != BLOCK_FALLBACK)
            st.blocks[i].dropped = true;
}

static char *tool_name(json name)
{
    span body = json_get_escaped_body(name);
    size_t prefix = strlen(st.tool_prefix);
    if (prefix && body.n > prefix && !memcmp(body.p, st.tool_prefix, prefix))
        body.p += prefix, body.n -= prefix;
    return span_dup(body);
}

static void start_block(json event)
{
    json start = json_get(event, "content_block");
    block b = {
        .kind = kind_of(json_get(start, "type")),
        .stream_index = (long)json_get_num(json_get(event, "index"), -1)
    };
    buf_set(&b.start_json, json_trim(start));
    buf_putspan(
        &b.escaped_text,
        json_get_escaped_body(json_get(start, b.kind == BLOCK_THINKING ? "thinking" : "text"))
    );
    buf_putspan(&b.escaped_signature, json_get_escaped_body(json_get(start, "signature")));
    if (b.kind == BLOCK_FALLBACK)
        drop_blocks_before_fallback();
    VEC_PUSH(st.blocks, st.nblocks, st.capblocks, b);
    if (b.kind != BLOCK_TOOL_USE)
        return;
    char *name = tool_name(json_get(start, "name"));
    provider_emit("{'type':'tool_use_start','id':%R,'name':'%r'}", json_get(start, "id"), name);
    free(name);
}

static void apply_delta(json event)
{
    block *b = block_for(event);
    json delta = json_get(event, "delta"), type = json_get(delta, "type");
    if (!b)
        return;
    if (json_eq(type, "text_delta"))
    {
        buf_putspan(&b->escaped_text, json_get_escaped_body(json_get(delta, "text")));
        provider_emit("{'type':'text_delta','text':%R}", json_get(delta, "text"));
    }
    else if (json_eq(type, "thinking_delta"))
    {
        span thinking = json_get_escaped_body(json_get(delta, "thinking"));
        buf_putspan(&b->escaped_text, thinking);
        if (thinking.n)
            provider_emit("{'type':'thinking_delta','text':%R}", json_get(delta, "thinking"));
    }
    else if (json_eq(type, "signature_delta"))
    {
        buf_putspan(&b->escaped_signature, json_get_escaped_body(json_get(delta, "signature")));
    }
    else if (json_eq(type, "input_json_delta"))
    {
        json_decode(json_get(delta, "partial_json"), &b->input_json);
        provider_emit(
            "{'type':'tool_use_delta','id':%R,'json':%R}",
            json_get(buf_span(&b->start_json), "id"),
            json_get(delta, "partial_json")
        );
    }
}

static void finish_message(json event)
{
    json reason = json_get(json_get(event, "delta"), "stop_reason");
    if (json_is(reason, '"'))
        buf_set(&st.escaped_stop_reason, json_get_escaped_body(reason));
    read_usage(json_get(event, "usage"));
}

bool anthropic_read_event(json event)
{
    json type = json_get(event, "type");
    if (json_eq(type, "message_start"))
        start_message(json_get(event, "message"));
    else if (json_eq(type, "content_block_start"))
        start_block(event);
    else if (json_eq(type, "content_block_delta"))
        apply_delta(event);
    else if (json_eq(type, "message_delta"))
        finish_message(event);
    else if (json_eq(type, "message_stop"))
        st.saw_message_stop = true;
    return !json_eq(type, "error");
}

bool anthropic_is_complete(void)
{
    return st.saw_message_stop;
}

static bool stop_is(const char *reason)
{
    return span_eq(buf_span(&st.escaped_stop_reason), reason);
}

static const char *stop_name(bool aborted)
{
    if (aborted)
        return "aborted";
    if (stop_is("tool_use") || stop_is("max_tokens") || stop_is("refusal"))
        return buf_cstr(&st.escaped_stop_reason);
    if (stop_is("model_context_window_exceeded"))
        return "max_tokens";
    return "end_turn";
}

static bool keep_block(const block *b, bool aborted)
{
    if (b->dropped)
        return false;
    switch (b->kind)
    {
    case BLOCK_TEXT:
        return b->escaped_text.n > 0;
    case BLOCK_THINKING:
        return b->escaped_signature.n > 0;
    case BLOCK_REDACTED:
    case BLOCK_FALLBACK:
        return true;
    case BLOCK_TOOL_USE:
        return !aborted && stop_is("tool_use");
    default:
        return !aborted;
    }
}

static void put_tool_input(json_buf *out, const block *b)
{
    json input = json_trim(buf_span(&b->input_json));
    if (!input.n)
        buf_puts(out, "{}");
    else if (json_is(input, '{') && json_is_valid(input))
        buf_putspan(out, input);
    else
        json_put_string(out, input);
}

static void put_block(json_buf *out, block *b)
{
    json start = buf_span(&b->start_json);
    if (b->kind == BLOCK_TEXT)
    {
        json_printf(out, "{'type':'text','text':'%r'}", buf_cstr(&b->escaped_text));
    }
    else if (b->kind == BLOCK_THINKING)
    {
        json_printf(
            out,
            "{'type':'thinking','thinking':'%r','signature':'%r'}",
            buf_cstr(&b->escaped_text),
            buf_cstr(&b->escaped_signature)
        );
    }
    else if (b->kind == BLOCK_TOOL_USE)
    {
        char *name = tool_name(json_get(start, "name"));
        json_printf(
            out,
            "{'type':'tool_use','id':%R,'name':'%r','input':",
            json_get(start, "id"),
            name
        );
        put_tool_input(out, b);
        buf_putc(out, '}');
        free(name);
    }
    else
    {
        buf_putspan(out, start);
    }
}

void anthropic_emit_result(bool aborted)
{
    json_buf content = {0};
    buf_putc(&content, '[');
    for (size_t i = 0; i < st.nblocks; i++)
    {
        if (!keep_block(&st.blocks[i], aborted))
            continue;
        json_put_comma_unless_first(&content);
        put_block(&content, &st.blocks[i]);
    }
    buf_putc(&content, ']');
    provider_emit_result(
        stop_name(aborted),
        buf_span(&content),
        (
            provider_usage
        ){st.usage_tokens[USAGE_INPUT],
            st.usage_tokens[USAGE_OUTPUT],
            st.usage_tokens[USAGE_CACHE_READ],
            st.usage_tokens[USAGE_CACHE_WRITE]}
    );
}
