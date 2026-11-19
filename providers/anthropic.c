#include "base.h"
#include "json.h"

#include <curl/curl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    ATTEMPTS                = 4,
    DEFAULT_MAX_TOKENS      = 64000,
    CONNECT_TIMEOUT_SECONDS = 30,
    STALL_TIMEOUT_SECONDS   = 300,
    FIRST_BACKOFF_TICKS     = 10,
    BACKOFF_TICK_NS         = 100000000,
};

typedef struct {
    const char *id;
    long long context_tokens, max_output_tokens;
    bool adaptive_thinking, server_fallbacks;
} model;

static const model MODELS[] = {
    { "claude-fable-5-2",  1000000, 128000, true,  true  },
    { "claude-opus-5-5",   1000000, 128000, true,  true  },
    { "claude-opus-5",     1000000, 128000, true,  true  },
    { "claude-opus-4-8",   1000000, 128000, true,  false },
    { "claude-sonnet-5-5", 1000000, 128000, true,  true  },
    { "claude-sonnet-5",   1000000, 128000, true,  false },
    { "claude-haiku-5-5",  1000000, 128000, true,  false },
    { "claude-haiku-4-5",   200000,  64000, false, false },
};

static const model UNKNOWN_MODEL = { NULL, 0, 0, true, false };

typedef enum { BLOCK_TEXT, BLOCK_THINKING, BLOCK_REDACTED, BLOCK_TOOL_USE, BLOCK_FALLBACK, BLOCK_OTHER } block_kind;

typedef struct {
    block_kind kind;
    long       stream_index;
    json_buf   start_json, input_json;
    buf        escaped_text, escaped_signature;
    bool       dropped;
} block;

typedef enum { COMPLETE, ABORTED, FAILED } outcome;
typedef enum { FINAL, RETRYABLE } retry_mode;

enum { USAGE_INPUT, USAGE_OUTPUT, USAGE_CACHE_READ, USAGE_CACHE_WRITE, NUSAGE };

static const char *const USAGE_KEYS[NUSAGE] = {
    "input_tokens", "output_tokens", "cache_read_input_tokens", "cache_creation_input_tokens",
};

static struct {
    volatile sig_atomic_t terminated;
    json_buf    request;
    buf         url, sse_pending, error_body, escaped_stop_reason;
    bool        beta_fallbacks, emitted_any, saw_message_stop;
    block      *blocks;
    size_t      nblocks, capblocks;
    long long   usage_tokens[NUSAGE];
    long        http_status;
    const char *fail_kind;
    char       *fail_message;
    retry_mode  fail_retry;
} st;

static void print_and_free_line(json_buf *line)
{
    buf_putc(line, '\n');
    fwrite(line->p, 1, line->n, stdout);
    fflush(stdout);
    buf_free(line);
}

static void emit(const char *fmt, ...)
{
    json_buf line = { 0 };
    va_list ap;
    va_start(ap, fmt);
    json_vprintf(&line, fmt, ap);
    va_end(ap);
    print_and_free_line(&line);
    st.emitted_any = true;
}

static const model *find_model(const char *id)
{
    for (size_t i = 0; i < LEN(MODELS); i++)
        if (!strcmp(MODELS[i].id, id)) return &MODELS[i];
    return &UNKNOWN_MODEL;
}

static int list_models(void)
{
    for (size_t i = 0; i < LEN(MODELS); i++)
        emit("{'id':%s,'context':%D,'max_output':%D}", MODELS[i].id, MODELS[i].context_tokens,
             MODELS[i].max_output_tokens);
    return 0;
}

static int emit_error(const char *kind, const char *message)
{
    emit("{'type':'error','kind':%s,'message':%s,'status':%D}", kind, message, (long long)st.http_status);
    return 1;
}

static span escaped_string_body(json string)
{
    return json_is(string, '"') && string.n >= 2 ? (span){ string.p + 1, string.n - 2 } : (span){ "", 0 };
}

static long long max_tokens(const model *m, json params)
{
    long long n = (long long)json_num(json_get(params, "max_tokens"), DEFAULT_MAX_TOKENS);
    if (n < 1) n = DEFAULT_MAX_TOKENS;
    return m->max_output_tokens && n > m->max_output_tokens ? m->max_output_tokens : n;
}

static json effort(json params)
{
    json e = json_get(params, "effort");
    return json_is(e, '"') ? e : S("\"high\"");
}

enum {
    MAX_IMAGE_BASE64_BYTES = 10 << 20,
    MAX_EDGE_PX            = 8000,
    MANY_IMAGES            = 20,
    MANY_IMAGES_EDGE_PX    = 3000,
    MEDIA_BUDGET_BYTES     = 100 << 20,
    IMAGE_HEADER_BYTES     = 256 << 10,
};

static const char *const ACCEPTED_MEDIA[] = { "image/jpeg", "image/png", "image/gif", "image/webp", "application/pdf" };

typedef struct {
    json block, base64, media_type;
    bool image;
    char omit_reason[160];
} media;

static struct {
    media  *v;
    size_t  n, cap, images, base64_bytes;
} found;

static unsigned be16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned le16(const unsigned char *p) { return (unsigned)p[1] << 8 | p[0]; }
static unsigned be32(const unsigned char *p) { return be16(p) << 16 | be16(p + 2); }
static unsigned le24(const unsigned char *p) { return (unsigned)p[2] << 16 | le16(p); }

static int sextet(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

static size_t decode_base64_prefix(span b64, unsigned char *out, size_t max)
{
    size_t n = 0;
    unsigned long bits = 0;
    int have = 0;
    for (size_t i = 0; i < b64.n && n < max; i++) {
        int v = sextet(b64.p[i]);
        if (v < 0) continue;
        bits = bits << 6 | (unsigned long)v;
        if ((have += 6) >= 8) out[n++] = (unsigned char)(bits >> (have -= 8));
    }
    return n;
}

static bool jpeg_size(const unsigned char *p, size_t n, unsigned *w, unsigned *h)
{
    for (size_t i = 2; i + 9 < n && p[i] == 0xFF;) {
        unsigned marker = p[i + 1];
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            *h = be16(p + i + 5);
            *w = be16(p + i + 7);
            return true;
        }
        i += 2 + be16(p + i + 2);
    }
    return false;
}

static bool webp_size(const unsigned char *p, size_t n, unsigned *w, unsigned *h)
{
    if (n < 30 || memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WEBP", 4) != 0) return false;
    if (memcmp(p + 12, "VP8 ", 4) == 0) {
        *w = le16(p + 26) & 0x3FFF;
        *h = le16(p + 28) & 0x3FFF;
    } else if (memcmp(p + 12, "VP8L", 4) == 0) {
        *w = 1 + (((p[22] & 0x3Fu) << 8) | p[21]);
        *h = 1 + (((p[24] & 0x0Fu) << 10) | (unsigned)p[23] << 2 | (p[22] & 0xC0u) >> 6);
    } else if (memcmp(p + 12, "VP8X", 4) == 0) {
        *w = 1 + le24(p + 24);
        *h = 1 + le24(p + 27);
    } else {
        return false;
    }
    return true;
}

static bool image_size(span base64, unsigned *w, unsigned *h)
{
    static unsigned char head[IMAGE_HEADER_BYTES];
    size_t n = decode_base64_prefix(base64, head, sizeof head);
    if (n >= 24 && memcmp(head, "\x89PNG", 4) == 0) {
        *w = be32(head + 16);
        *h = be32(head + 20);
        return true;
    }
    if (n >= 10 && memcmp(head, "GIF8", 4) == 0) {
        *w = le16(head + 6);
        *h = le16(head + 8);
        return true;
    }
    if (n >= 4 && head[0] == 0xFF && head[1] == 0xD8) return jpeg_size(head, n, w, h);
    return webp_size(head, n, w, h);
}

static void set_omit_reason(media *m, const char *reason)
{
    if (!m->omit_reason[0]) snprintf(m->omit_reason, sizeof m->omit_reason, "%s", reason);
}

static void record_media(json block)
{
    json type = json_get(block, "type"), source = json_get(block, "source");
    bool image = json_eq(type, "image");
    if (!(image || json_eq(type, "document")) || !json_eq(json_get(source, "type"), "base64")) return;
    json base64 = json_get(source, "data");
    media m = { .block = block, .base64 = base64, .media_type = json_get(source, "media_type"), .image = image };
    VEC_PUSH(found.v, found.n, found.cap, m);
    found.images += image;
    found.base64_bytes += base64.n;
}

static void for_each_block(json content, void (*fn)(json))
{
    jiter it = json_iter(content);
    for (json block; json_next(&it, NULL, &block);) {
        fn(block);
        if (json_eq(json_get(block, "type"), "tool_result")) for_each_block(json_get(block, "content"), fn);
    }
}

static bool accepted(json media_type)
{
    for (size_t i = 0; i < LEN(ACCEPTED_MEDIA); i++)
        if (json_eq(media_type, ACCEPTED_MEDIA[i])) return true;
    return false;
}

static void judge_media_limits(void)
{
    unsigned w, h;
    for (size_t i = 0; i < found.n; i++) {
        media *m = &found.v[i];
        if (!accepted(m->media_type)) {
            span type = escaped_string_body(m->media_type);
            snprintf(m->omit_reason, sizeof m->omit_reason, "%.*s is not a type the API accepts", (int)type.n, type.p);
            continue;
        }
        if (!m->image) continue;
        if (m->base64.n > MAX_IMAGE_BASE64_BYTES) set_omit_reason(m, "larger than the API's 10 MB limit");
        else if (image_size(m->base64, &w, &h)) {
            unsigned edge = w > h ? w : h;
            if (edge > MAX_EDGE_PX) set_omit_reason(m, "larger than the API's 8000 px limit");
            else if (found.images > MANY_IMAGES && edge > MANY_IMAGES_EDGE_PX)
                set_omit_reason(m, "larger than 3000 px, the API's limit when a request has more than 20 images");
        }
    }
    for (size_t i = 0; i < found.n && found.base64_bytes > MEDIA_BUDGET_BYTES; i++) {
        found.base64_bytes -= found.v[i].base64.n;
        set_omit_reason(&found.v[i], "dropped, oldest first, to keep the request under the API's size limit");
    }
}

static const media *omitted_media_at(const char *block_start)
{
    for (size_t i = 0; i < found.n; i++)
        if (found.v[i].block.p == block_start && found.v[i].omit_reason[0]) return &found.v[i];
    return NULL;
}

static bool contains_omitted_media(json v)
{
    for (size_t i = 0; i < found.n; i++)
        if (found.v[i].omit_reason[0] && found.v[i].block.p >= v.p && found.v[i].block.p < v.p + v.n) return true;
    return false;
}

static void comma_unless_first(json_buf *array)
{
    if (array->n && array->p[array->n - 1] != '[') buf_putc(array, ',');
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
        comma_unless_first(out);
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

static void put_messages_within_limits(json_buf *out, json messages)
{
    jiter it = json_iter(messages);
    for (json message; json_next(&it, NULL, &message);) for_each_block(json_get(message, "content"), record_media);
    judge_media_limits();
    put_array_within_limits(out, json_trim(messages));
    free(found.v);
    memset(&found, 0, sizeof found);
}

static void describe_running(buf *text, json pending)
{
    json pid = json_get(pending, "pid"), stdout_path = json_get(pending, "out"), stderr_path = json_get(pending, "err");
    buf_puts(text, "Still running in the background");
    if (json_num(pid, 0) > 0) buf_printf(text, " (pid %.0f, its own process group)", json_num(pid, 0));
    if (json_is(stdout_path, '"') || json_is(stderr_path, '"')) {
        buf_puts(text, "; its output so far is in ");
        json_text(stdout_path, text);
        if (json_is(stdout_path, '"') && json_is(stderr_path, '"')) buf_puts(text, " and ");
        json_text(stderr_path, text);
    }
    buf_puts(text, ". Its result arrives later as a message.");
    if (json_true(json_get(pending, "stdin_open"))) {
        buf_puts(text, " Its stdin is open: send it input with write and job \"");
        json_text(json_get(pending, "tool_use_id"), text);
        buf_puts(text, "\".");
    }
}

static void put_pending(json_buf *out, json pending)
{
    buf text = { 0 };
    if (json_eq(json_get(pending, "state"), "running")) describe_running(&text, pending);
    else buf_puts(&text, "Not finished yet; its result arrives later as a message.");
    json_printf(out, "{'type':'tool_result','tool_use_id':%R,'content':[{'type':'text','text':%S}]}",
         json_get(pending, "tool_use_id"), buf_span(&text));
    buf_free(&text);
}

static void put_attribute(buf *text, const char *name, json value)
{
    if (!json_is(value, '"')) return;
    buf_printf(text, " %s=\"", name);
    json_text(value, text);
    buf_putc(text, '"');
}

static bool is_text(json block)
{
    return json_eq(json_get(block, "type"), "text");
}

static void put_job_result(json_buf *out, json result)
{
    json content = json_get(result, "content"), ran_seconds = json_get(result, "ran");
    buf text = { 0 };
    buf_puts(&text, "<job");
    put_attribute(&text, "call", json_get(result, "tool_use_id"));
    put_attribute(&text, "name", json_get(result, "name"));
    put_attribute(&text, "status", json_get(result, "status"));
    if (ran_seconds.p) buf_printf(&text, " ran=\"%.0fs\"", json_num(ran_seconds, 0));
    buf_puts(&text, ">\n");
    json_text(content, &text);
    jiter it = json_iter(content);
    for (json block; json_next(&it, NULL, &block);)
        if (is_text(block)) json_text(json_get(block, "text"), &text);
    buf_puts(&text, "\n</job>");
    json_printf(out, "{'type':'text','text':%S}", buf_span(&text));
    it = json_iter(content);
    for (json block; json_next(&it, NULL, &block);)
        if (!is_text(block)) buf_putc(out, ','), buf_putspan(out, block);
    buf_free(&text);
}

static void put_tool_result(json_buf *out, json result)
{
    jiter it = json_iter(result);
    bool first = true;
    buf_putc(out, '{');
    for (json k, v; json_next(&it, &k, &v);) {
        if (json_eq(k, "name")) continue;
        json_printf(out, "%r%R:%R", first ? "" : ",", k, v);
        first = false;
    }
    buf_putc(out, '}');
}

static bool answers_call(json block)
{
    json type = json_get(block, "type");
    return json_eq(type, "tool_pending") || json_eq(type, "tool_result");
}

static void put_user_block(json_buf *out, json block)
{
    json type = json_get(block, "type");
    comma_unless_first(out);
    if (json_eq(type, "tool_pending")) put_pending(out, block);
    else if (json_eq(type, "tool_result")) put_tool_result(out, block);
    else if (json_eq(type, "job_result")) put_job_result(out, block);
    else buf_putspan(out, block);
}

static bool is_user(json message)
{
    return json_eq(json_get(message, "role"), "user");
}

typedef enum { OTHER_BLOCKS, CALL_ANSWERS } user_block_group;

static void put_user_blocks(json_buf *out, jiter messages, user_block_group group)
{
    bool answers = group == CALL_ANSWERS;
    for (json message; json_next(&messages, NULL, &message) && is_user(message);) {
        json content = json_get(message, "content");
        if (json_is(content, '"') && !answers)
            comma_unless_first(out), json_printf(out, "{'type':'text','text':%R}", content);
        jiter it = json_iter(content);
        for (json block; json_next(&it, NULL, &block);)
            if (answers_call(block) == answers) put_user_block(out, block);
    }
}

static void put_tool_use(json_buf *out, json tool_use)
{
    jiter it = json_iter(tool_use);
    bool first = true;
    buf_putc(out, '{');
    for (json k, v; json_next(&it, &k, &v); first = false) {
        json_printf(out, "%r%R:", first ? "" : ",", k);
        if (json_eq(k, "input") && !json_is(v, '{')) buf_puts(out, "{}");
        else buf_putspan(out, v);
    }
    buf_putc(out, '}');
}

static void put_assistant(json_buf *out, json message)
{
    json content = json_get(message, "content");
    if (!json_is(content, '[')) {
        buf_putspan(out, message);
        return;
    }
    buf_puts(out, "{\"role\":\"assistant\",\"content\":[");
    jiter it = json_iter(content);
    for (json block; json_next(&it, NULL, &block);) {
        comma_unless_first(out);
        if (json_eq(json_get(block, "type"), "tool_use")) put_tool_use(out, block);
        else buf_putspan(out, block);
    }
    buf_puts(out, "]}");
}

static void put_rendered_messages(json_buf *out, json messages)
{
    jiter it = json_iter(messages), at = it;
    bool merging = false;
    buf_putc(out, '[');
    for (json message; json_next(&it, NULL, &message); at = it) {
        bool user = is_user(message);
        if (user && merging) continue;
        merging = user;
        comma_unless_first(out);
        if (!user) {
            put_assistant(out, message);
            continue;
        }
        buf_puts(out, "{\"role\":\"user\",\"content\":[");
        put_user_blocks(out, at, CALL_ANSWERS);
        put_user_blocks(out, at, OTHER_BLOCKS);
        buf_puts(out, "]}");
    }
    buf_putc(out, ']');
}

static void put_tool(json_buf *b, json tool)
{
    json_printf(b, "{'name':%R,'description':%R,'input_schema':%R,'eager_input_streaming':true}",
                json_get(tool, "name"), json_get(tool, "description"), json_get(tool, "input_schema"));
}

static void put_tools(json_buf *b, json tools)
{
    jiter it = json_iter(tools);
    json tool;
    buf_puts(b, ",\"tools\":[");
    for (int i = 0; json_next(&it, NULL, &tool); i++) {
        if (i) buf_putc(b, ',');
        put_tool(b, tool);
    }
    buf_putc(b, ']');
}

static bool build_request(json kernel_request)
{
    json model_id = json_get(kernel_request, "model"), system = json_get(kernel_request, "system");
    json messages = json_get(kernel_request, "messages"), tools = json_get(kernel_request, "tools");
    json params = json_get(kernel_request, "params");
    if (!json_is(model_id, '"') || !json_is(messages, '[')) return false;
    char *id = json_text_dup(model_id);
    const model *m = find_model(id);
    free(id);
    st.beta_fallbacks = m->server_fallbacks && !json_eq(json_get(params, "fallbacks"), "off");
    json_buf *b = &st.request;
    json_printf(b, "{'model':%R,'max_tokens':%D,'stream':true", model_id, max_tokens(m, params));
    if (!json_null_or_absent(system) && !json_eq(system, "")) json_printf(b, ",'system':%R", system);
    json_buf rendered = { 0 };
    put_rendered_messages(&rendered, messages);
    buf_puts(b, ",\"messages\":");
    put_messages_within_limits(b, buf_span(&rendered));
    buf_free(&rendered);
    if (json_is(tools, '[') && tools.n > 2) put_tools(b, tools);
    if (m->adaptive_thinking)
        json_printf(b, ",'thinking':{'type':'adaptive','display':'summarized'},'output_config':{'effort':%R}",
                    effort(params));
    if (st.beta_fallbacks) json_printf(b, ",'fallbacks':'default'");
    json_printf(b, ",'cache_control':{'type':'ephemeral'}}");
    return true;
}

static void set_url(void)
{
    const char *base = getenv("ANTHROPIC_BASE_URL");
    buf_puts(&st.url, base && *base ? base : "https://api.anthropic.com");
    while (st.url.n && st.url.p[st.url.n - 1] == '/') st.url.n--;
    buf_puts(&st.url, "/v1/messages");
}

static void read_usage(json usage)
{
    for (size_t i = 0; i < NUSAGE; i++) {
        json v = json_get(usage, USAGE_KEYS[i]);
        if (v.p && !json_null_or_absent(v)) st.usage_tokens[i] = (long long)json_num(v, 0);
    }
}

static block_kind kind_of(json type)
{
    if (json_eq(type, "text")) return BLOCK_TEXT;
    if (json_eq(type, "thinking")) return BLOCK_THINKING;
    if (json_eq(type, "redacted_thinking")) return BLOCK_REDACTED;
    if (json_eq(type, "tool_use")) return BLOCK_TOOL_USE;
    if (json_eq(type, "fallback")) return BLOCK_FALLBACK;
    return BLOCK_OTHER;
}

static block *block_for(json event)
{
    long stream_index = (long)json_num(json_get(event, "index"), -1);
    for (size_t i = st.nblocks; i-- > 0;)
        if (st.blocks[i].stream_index == stream_index) return &st.blocks[i];
    return NULL;
}

static void drop_blocks_before_fallback(void)
{
    for (size_t i = 0; i < st.nblocks; i++)
        if (st.blocks[i].kind != BLOCK_TEXT && st.blocks[i].kind != BLOCK_FALLBACK) st.blocks[i].dropped = true;
}

static void start_block(json event)
{
    json start = json_get(event, "content_block");
    block b = { .kind = kind_of(json_get(start, "type")),
                .stream_index = (long)json_num(json_get(event, "index"), -1) };
    buf_set(&b.start_json, json_trim(start));
    buf_putspan(&b.escaped_text, escaped_string_body(json_get(start, b.kind == BLOCK_THINKING ? "thinking" : "text")));
    buf_putspan(&b.escaped_signature, escaped_string_body(json_get(start, "signature")));
    if (b.kind == BLOCK_FALLBACK) drop_blocks_before_fallback();
    VEC_PUSH(st.blocks, st.nblocks, st.capblocks, b);
    if (b.kind == BLOCK_TOOL_USE)
        emit("{'type':'tool_use_start','id':%R,'name':%R}", json_get(start, "id"), json_get(start, "name"));
}

static void apply_delta(json event)
{
    block *b = block_for(event);
    json delta = json_get(event, "delta"), type = json_get(delta, "type");
    if (!b) return;
    if (json_eq(type, "text_delta")) {
        buf_putspan(&b->escaped_text, escaped_string_body(json_get(delta, "text")));
        emit("{'type':'text_delta','text':%R}", json_get(delta, "text"));
    } else if (json_eq(type, "thinking_delta")) {
        buf_putspan(&b->escaped_text, escaped_string_body(json_get(delta, "thinking")));
        emit("{'type':'thinking_delta','text':%R}", json_get(delta, "thinking"));
    } else if (json_eq(type, "signature_delta")) {
        buf_putspan(&b->escaped_signature, escaped_string_body(json_get(delta, "signature")));
    } else if (json_eq(type, "input_json_delta")) {
        json_text(json_get(delta, "partial_json"), &b->input_json);
        emit("{'type':'tool_use_delta','id':%R,'json':%R}",
             json_get(buf_span(&b->start_json), "id"), json_get(delta, "partial_json"));
    }
}

static void finish_message(json event)
{
    json reason = json_get(json_get(event, "delta"), "stop_reason");
    if (json_is(reason, '"')) buf_set(&st.escaped_stop_reason, escaped_string_body(reason));
    read_usage(json_get(event, "usage"));
}

static void on_sse_event(json event)
{
    json type = json_get(event, "type");
    if (json_eq(type, "message_start")) read_usage(json_get(json_get(event, "message"), "usage"));
    else if (json_eq(type, "content_block_start")) start_block(event);
    else if (json_eq(type, "content_block_delta")) apply_delta(event);
    else if (json_eq(type, "message_delta")) finish_message(event);
    else if (json_eq(type, "message_stop")) st.saw_message_stop = true;
    else if (json_eq(type, "error")) buf_set(&st.error_body, event);
}

static void consume_sse_lines(void)
{
    buf *pending = &st.sse_pending;
    char *nl;
    while (pending->n && (nl = memchr(pending->p, '\n', pending->n))) {
        size_t n = (size_t)(nl - pending->p);
        span line = { pending->p, n && nl[-1] == '\r' ? n - 1 : n };
        if (line.n > 5 && !memcmp(line.p, "data:", 5))
            on_sse_event(json_trim((span){ line.p + 5, line.n - 5 }));
        buf_drop_front(pending, n + 1);
    }
}

static size_t on_data(char *bytes, size_t size, size_t n, void *curl)
{
    size_t len = size * n;
    if (!st.http_status) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &st.http_status);
    if (st.http_status != 200) {
        buf_put(&st.error_body, bytes, len);
        return len;
    }
    buf_put(&st.sse_pending, bytes, len);
    consume_sse_lines();
    return len;
}

static int on_progress(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    (void)clientp, (void)dltotal, (void)dlnow, (void)ultotal, (void)ulnow;
    return st.terminated ? 1 : 0;
}

static bool stop_is(const char *reason)
{
    return span_eq(buf_span(&st.escaped_stop_reason), reason);
}

static const char *stop_name(outcome o)
{
    if (o == ABORTED) return "aborted";
    if (stop_is("tool_use") || stop_is("max_tokens") || stop_is("refusal")) return buf_cstr(&st.escaped_stop_reason);
    if (stop_is("model_context_window_exceeded")) return "max_tokens";
    return "end_turn";
}

static bool keep_block(const block *b, outcome o)
{
    if (b->dropped) return false;
    switch (b->kind) {
    case BLOCK_TEXT:     return b->escaped_text.n > 0;
    case BLOCK_THINKING: return b->escaped_signature.n > 0;
    case BLOCK_REDACTED:
    case BLOCK_FALLBACK: return true;
    case BLOCK_TOOL_USE: return o != ABORTED && stop_is("tool_use");
    default:             return o != ABORTED;
    }
}

static void put_tool_input(json_buf *out, const block *b)
{
    json input = json_trim(buf_span(&b->input_json));
    if (!input.n) buf_puts(out, "{}");
    else if (json_is(input, '{') && json_valid(input)) buf_putspan(out, input);
    else json_put_string(out, input);
}

static void put_block(json_buf *out, block *b)
{
    json start = buf_span(&b->start_json);
    if (b->kind == BLOCK_TEXT)
        json_printf(out, "{'type':'text','text':'%r'}", buf_cstr(&b->escaped_text));
    else if (b->kind == BLOCK_THINKING)
        json_printf(out, "{'type':'thinking','thinking':'%r','signature':'%r'}",
                    buf_cstr(&b->escaped_text), buf_cstr(&b->escaped_signature));
    else if (b->kind == BLOCK_TOOL_USE) {
        json_printf(out, "{'type':'tool_use','id':%R,'name':%R,'input':", json_get(start, "id"), json_get(start, "name"));
        put_tool_input(out, b);
        buf_putc(out, '}');
    } else
        buf_putspan(out, start);
}

static void put_content(json_buf *out, outcome o)
{
    bool first = true;
    buf_putc(out, '[');
    for (size_t i = 0; i < st.nblocks; i++) {
        if (!keep_block(&st.blocks[i], o)) continue;
        if (!first) buf_putc(out, ',');
        put_block(out, &st.blocks[i]);
        first = false;
    }
    buf_putc(out, ']');
}

static int emit_result(outcome o)
{
    json_buf line = { 0 };
    json_printf(&line, "{'type':'result','stop':%s,'message':{'role':'assistant','content':", stop_name(o));
    put_content(&line, o);
    json_printf(&line, "},'usage':{'input':%D,'output':%D,'cache_read':%D,'cache_write':%D}}",
                st.usage_tokens[USAGE_INPUT], st.usage_tokens[USAGE_OUTPUT],
                st.usage_tokens[USAGE_CACHE_READ], st.usage_tokens[USAGE_CACHE_WRITE]);
    print_and_free_line(&line);
    return 0;
}

static void reset_attempt(void)
{
    for (size_t i = 0; i < st.nblocks; i++) {
        buf_free(&st.blocks[i].start_json);
        buf_free(&st.blocks[i].escaped_text);
        buf_free(&st.blocks[i].escaped_signature);
        buf_free(&st.blocks[i].input_json);
    }
    st.nblocks = 0;
    st.sse_pending.n = st.error_body.n = st.escaped_stop_reason.n = 0;
    memset(st.usage_tokens, 0, sizeof st.usage_tokens);
    st.http_status = 0;
    st.saw_message_stop = false;
}

static outcome failed(const char *kind, char *owned_message, retry_mode retry)
{
    st.fail_kind = kind;
    st.fail_message = owned_message;
    st.fail_retry = retry;
    return FAILED;
}

static bool mentions_overflow(const char *message)
{
    return strstr(message, "prompt is too long") || strstr(message, "context limit") || strstr(message, "context window");
}

static const char *error_kind(json type, const char *message)
{
    long s = st.http_status;
    if (json_eq(type, "authentication_error") || json_eq(type, "permission_error") || s == 401 || s == 403) return "auth";
    if (json_eq(type, "rate_limit_error") || s == 429) return "rate_limit";
    if (json_eq(type, "overloaded_error") || s == 529) return "overloaded";
    if (json_eq(type, "request_too_large") || s == 413 || mentions_overflow(message)) return "context_overflow";
    if (json_eq(type, "invalid_request_error") || json_eq(type, "not_found_error") || (s >= 400 && s < 500)) return "invalid_request";
    return "other";
}

static outcome api_failure(void)
{
    json error = json_get(buf_span(&st.error_body), "error");
    json type = json_get(error, "type"), message = json_get(error, "message");
    char *text = json_is(message, '"') ? json_text_dup(message) : xstrdup("unexpected error response");
    const char *kind = error_kind(type, text);
    bool retryable = !strcmp(kind, "rate_limit") || !strcmp(kind, "overloaded") || st.http_status >= 500
                  || json_eq(type, "api_error");
    return failed(kind, text, retryable ? RETRYABLE : FINAL);
}

static struct curl_slist *headers(void)
{
    const char *key = getenv("ANTHROPIC_API_KEY");
    buf auth = { 0 };
    buf_printf(&auth, "x-api-key: %s", key ? key : "");
    struct curl_slist *h = curl_slist_append(NULL, "content-type: application/json");
    h = curl_slist_append(h, "anthropic-version: 2026-10-21");
    h = curl_slist_append(h, buf_cstr(&auth));
    if (st.beta_fallbacks) h = curl_slist_append(h, "anthropic-beta: server-side-fallback-2026-07-01");
    buf_free(&auth);
    return h;
}

static CURLcode perform(char *curl_error)
{
    CURL *c = curl_easy_init();
    struct curl_slist *h = headers();
    curl_easy_setopt(c, CURLOPT_URL, buf_cstr(&st.url));
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, st.request.p);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)st.request.n);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, c);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, curl_error);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, (long)CONNECT_TIMEOUT_SECONDS);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, (long)STALL_TIMEOUT_SECONDS);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &st.http_status);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return rc;
}

static outcome transfer(void)
{
    char curl_error[CURL_ERROR_SIZE] = "";
    CURLcode rc = perform(curl_error);
    if (st.terminated) return ABORTED;
    if (rc != CURLE_OK) return failed("network", xstrdup(*curl_error ? curl_error : curl_easy_strerror(rc)), RETRYABLE);
    if (st.http_status != 200 || st.error_body.n) return api_failure();
    if (!st.saw_message_stop) return failed("network", xstrdup("stream ended before message_stop"), RETRYABLE);
    return COMPLETE;
}

static bool backoff(int attempt)
{
    struct timespec tick = { 0, BACKOFF_TICK_NS };
    for (int i = 0; i < FIRST_BACKOFF_TICKS << (attempt - 1) && !st.terminated; i++) nanosleep(&tick, NULL);
    return !st.terminated;
}

static int run(void)
{
    for (int attempt = 1;; attempt++) {
        reset_attempt();
        if (st.terminated) return emit_result(ABORTED);
        outcome o = transfer();
        if (o != FAILED) return emit_result(o);
        if (st.fail_retry == FINAL || st.emitted_any || attempt == ATTEMPTS || !backoff(attempt))
            return st.terminated ? emit_result(ABORTED) : emit_error(st.fail_kind, st.fail_message);
    }
}

static void on_sigterm(int sig)
{
    (void)sig;
    st.terminated = 1;
}

static void trap_sigterm(void)
{
    struct sigaction sa = { 0 };
    sa.sa_handler = on_sigterm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
}

static void read_all(FILE *f, buf *bytes)
{
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_put(bytes, chunk, n);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--models")) return list_models();
    trap_sigterm();
    json_buf kernel_request = { 0 };
    read_all(stdin, &kernel_request);
    if (!json_valid(buf_span(&kernel_request)) || !build_request(buf_span(&kernel_request)))
        return emit_error("invalid_request", "expected {\"model\",\"messages\",...} on stdin");
    if (!getenv("ANTHROPIC_API_KEY")) return emit_error("auth", "ANTHROPIC_API_KEY is not set");
    set_url();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    return run();
}
