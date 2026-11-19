#include "base.h"
#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum { CONTEXT_TOKENS = 1000000, MAX_OUTPUT_TOKENS = 128000, ACK_TIMEOUT_MS = 60000 };

static const char *const MODELS[] = { "fable", "opus", "sonnet", "haiku" };

static const char TOOL_PREFIX[] = "mcp__akern__";

static const char *const CLAUDE_ARGS[] = {
    "claude", "-p", "--input-format", "stream-json", "--output-format", "stream-json", "--verbose",
    "--include-partial-messages", "--no-session-persistence", "--tools", "", "--strict-mcp-config",
    "--setting-sources", "", "--disable-slash-commands", "--permission-mode", "dontAsk", "--max-turns", "1",
};

static const char *const CLAUDE_ENV[][2] = {
    { "CLAUDE_CODE_DISABLE_CLAUDE_MDS", "1" },
    { "CLAUDE_CODE_DISABLE_AUTO_MEMORY", "1" },
    { "CLAUDE_CODE_DISABLE_ADVISOR_TOOL", "1" },
    { "DISABLE_COMPACT", "1" },
};

static const char *const KEPT_CLAUDE_ENV[] = {
    "CLAUDE_CONFIG_DIR", "CLAUDE_CODE_OAUTH_TOKEN", "CLAUDE_CODE_USE_BEDROCK", "CLAUDE_CODE_USE_VERTEX", "CLAUDE_CODE_USE_FOUNDRY",
};

typedef enum { BLOCK_TEXT, BLOCK_THINKING, BLOCK_REDACTED, BLOCK_TOOL_USE, BLOCK_OTHER } block_kind;

typedef struct {
    block_kind kind;
    long       stream_index;
    json_buf   start_json, input_json;
    buf        escaped_text, escaped_signature;
} block;

typedef enum { COMPLETE, ABORTED } outcome;

enum { USAGE_INPUT, USAGE_OUTPUT, USAGE_CACHE_READ, USAGE_CACHE_WRITE, NUSAGE };

static const char *const USAGE_KEYS[NUSAGE] = {
    "input_tokens", "output_tokens", "cache_read_input_tokens", "cache_creation_input_tokens",
};

static struct {
    volatile sig_atomic_t terminated;
    buf         pending, escaped_stop_reason;
    json_buf    final_result;
    bool        saw_message_stop;
    size_t     *pauses;
    size_t      npauses, cappauses, results;
    const char *failure;
    block      *blocks;
    size_t      nblocks, capblocks;
    long long   usage_tokens[NUSAGE];
    long        api_status;
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
}

static int list_models(void)
{
    for (size_t i = 0; i < LEN(MODELS); i++)
        emit("{'id':%s,'context':%d,'max_output':%d}", MODELS[i], CONTEXT_TOKENS, MAX_OUTPUT_TOKENS);
    return 0;
}

static int emit_error(const char *kind, const char *message)
{
    emit("{'type':'error','kind':%s,'message':%s,'status':%D}", kind, message, (long long)st.api_status);
    return 1;
}

static span escaped_string_body(json string)
{
    return json_is(string, '"') && string.n >= 2 ? (span){ string.p + 1, string.n - 2 } : (span){ "", 0 };
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
            buf_putspan(out, message);
            continue;
        }
        buf_puts(out, "{\"role\":\"user\",\"content\":[");
        put_user_blocks(out, at, CALL_ANSWERS);
        put_user_blocks(out, at, OTHER_BLOCKS);
        buf_puts(out, "]}");
    }
    buf_putc(out, ']');
}

static void put_tool_use_for_claude(json_buf *out, json tool_use)
{
    jiter it = json_iter(tool_use);
    bool first = true;
    buf_putc(out, '{');
    for (json k, v; json_next(&it, &k, &v); first = false) {
        span name = escaped_string_body(v);
        json_printf(out, "%r%R:", first ? "" : ",", k);
        if (json_eq(k, "name")) buf_printf(out, "\"%s%.*s\"", TOOL_PREFIX, (int)name.n, name.p);
        else if (json_eq(k, "input") && !json_is(v, '{')) buf_puts(out, "{}");
        else buf_putspan(out, v);
    }
    buf_putc(out, '}');
}

static void put_assistant_for_claude(json_buf *out, json message)
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
        if (json_eq(json_get(block, "type"), "tool_use")) put_tool_use_for_claude(out, block);
        else buf_putspan(out, block);
    }
    buf_puts(out, "]}");
}

static void put_input_lines(buf *lines, json messages)
{
    jiter it = json_iter(messages);
    json message, next;
    for (bool more = json_next(&it, NULL, &message); more; message = next) {
        more = json_next(&it, NULL, &next);
        if (is_user(message)) {
            json_printf(lines, "{'type':'user',%r'message':%R}\n", more ? "\"shouldQuery\":false," : "", message);
            if (more) VEC_PUSH(st.pauses, st.npauses, st.cappauses, lines->n);
            continue;
        }
        buf_puts(lines, "{\"type\":\"assistant\",\"message\":");
        put_assistant_for_claude(lines, message);
        buf_puts(lines, "}\n");
    }
}

static void put_mcp_config(json_buf *out, const char *self, json tools)
{
    json_buf listed = { 0 };
    jiter it = json_iter(tools);
    buf_putc(&listed, '[');
    for (json tool; json_next(&it, NULL, &tool);) {
        comma_unless_first(&listed);
        json_printf(&listed, "{'name':%R,'description':%R,'inputSchema':%R}",
                    json_get(tool, "name"), json_get(tool, "description"), json_get(tool, "input_schema"));
    }
    buf_putc(&listed, ']');
    json_printf(out, "{'mcpServers':{'akern':{'command':%s,'args':['--mcp',%S]}}}", self, buf_span(&listed));
    buf_free(&listed);
}

static char *self_path(const char *argv0)
{
    char resolved[PATH_MAX];
    return xstrdup(realpath(argv0, resolved) ? resolved : argv0);
}

static bool build_invocation(json request, const char *argv0, strv *args, buf *input)
{
    json model_id = json_get(request, "model"), messages = json_get(request, "messages");
    json system = json_get(request, "system"), effort = json_get(json_get(request, "params"), "effort");
    if (!json_is(model_id, '"') || !json_is(messages, '[')) return false;
    for (size_t i = 0; i < LEN(CLAUDE_ARGS); i++) strv_push(args, xstrdup(CLAUDE_ARGS[i]));
    strv_push(args, xstrdup("--model"));
    strv_push(args, json_text_dup(model_id));
    strv_push(args, xstrdup("--system-prompt"));
    strv_push(args, json_is(system, '"') ? json_text_dup(system) : xstrdup(""));
    if (json_is(effort, '"')) strv_push(args, xstrdup("--effort")), strv_push(args, json_text_dup(effort));
    char *self = self_path(argv0);
    json_buf config = { 0 };
    put_mcp_config(&config, self, json_get(request, "tools"));
    strv_push(args, xstrdup("--mcp-config"));
    strv_push(args, xstrdup(buf_cstr(&config)));
    strv_push(args, NULL);
    json_buf rendered = { 0 }, limited = { 0 };
    put_rendered_messages(&rendered, messages);
    put_messages_within_limits(&limited, buf_span(&rendered));
    put_input_lines(input, buf_span(&limited));
    free(self);
    buf_free(&config);
    buf_free(&rendered);
    buf_free(&limited);
    return true;
}

static bool host_session_variable(span name)
{
    if (name.n < 6 || memcmp(name.p, "CLAUDE", 6)) return false;
    for (size_t i = 0; i < LEN(KEPT_CLAUDE_ENV); i++)
        if (span_eq(name, KEPT_CLAUDE_ENV[i])) return false;
    return true;
}

static void drop_host_session_env(void)
{
    extern char **environ;
    strv names = { 0 };
    for (char **e = environ; *e; e++) {
        span name = { *e, strcspn(*e, "=") };
        if (host_session_variable(name)) strv_push(&names, span_dup(name));
    }
    for (size_t i = 0; i < names.n; i++) unsetenv(names.v[i]);
}

static void set_claude_env(json params)
{
    drop_host_session_env();
    for (size_t i = 0; i < LEN(CLAUDE_ENV); i++) setenv(CLAUDE_ENV[i][0], CLAUDE_ENV[i][1], 1);
    json max_tokens = json_get(params, "max_tokens");
    if (json_num(max_tokens, 0) >= 1) {
        char text[32];
        snprintf(text, sizeof text, "%.0f", json_num(max_tokens, 0));
        setenv("CLAUDE_CODE_MAX_OUTPUT_TOKENS", text, 1);
    }
}

static pid_t start_claude(char **args, json params, int *input, int *output)
{
    int in[2], out[2];
    if (pipe(in) || pipe(out)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(in[0], 0), dup2(out[1], 1);
        close(in[0]), close(in[1]), close(out[0]), close(out[1]);
        signal(SIGPIPE, SIG_DFL);
        set_claude_env(params);
        execvp(args[0], args);
        dprintf(2, "%s: %s\n", args[0], strerror(errno));
        _exit(127);
    }
    close(in[0]), close(out[1]);
    fcntl(in[1], F_SETFL, O_NONBLOCK);
    *input = in[1];
    *output = out[0];
    return pid;
}

static void read_usage(json usage)
{
    for (size_t i = 0; i < NUSAGE; i++) {
        json v = json_get(usage, USAGE_KEYS[i]);
        if (v.p && !json_null_or_absent(v)) st.usage_tokens[i] = (long long)json_num(v, 0);
    }
}

static void reset_message(void)
{
    for (size_t i = 0; i < st.nblocks; i++) {
        buf_free(&st.blocks[i].start_json);
        buf_free(&st.blocks[i].escaped_text);
        buf_free(&st.blocks[i].escaped_signature);
        buf_free(&st.blocks[i].input_json);
    }
    st.nblocks = 0;
    st.escaped_stop_reason.n = 0;
    memset(st.usage_tokens, 0, sizeof st.usage_tokens);
    st.saw_message_stop = false;
}

static block_kind kind_of(json type)
{
    if (json_eq(type, "text")) return BLOCK_TEXT;
    if (json_eq(type, "thinking")) return BLOCK_THINKING;
    if (json_eq(type, "redacted_thinking")) return BLOCK_REDACTED;
    if (json_eq(type, "tool_use")) return BLOCK_TOOL_USE;
    return BLOCK_OTHER;
}

static block *block_for(json event)
{
    long stream_index = (long)json_num(json_get(event, "index"), -1);
    for (size_t i = st.nblocks; i-- > 0;)
        if (st.blocks[i].stream_index == stream_index) return &st.blocks[i];
    return NULL;
}

static char *tool_name(json name)
{
    span body = escaped_string_body(name);
    size_t prefix = sizeof TOOL_PREFIX - 1;
    if (body.n > prefix && !memcmp(body.p, TOOL_PREFIX, prefix)) body.p += prefix, body.n -= prefix;
    return span_dup(body);
}

static void start_block(json event)
{
    json start = json_get(event, "content_block");
    block b = { .kind = kind_of(json_get(start, "type")),
                .stream_index = (long)json_num(json_get(event, "index"), -1) };
    buf_set(&b.start_json, json_trim(start));
    buf_putspan(&b.escaped_text, escaped_string_body(json_get(start, b.kind == BLOCK_THINKING ? "thinking" : "text")));
    buf_putspan(&b.escaped_signature, escaped_string_body(json_get(start, "signature")));
    VEC_PUSH(st.blocks, st.nblocks, st.capblocks, b);
    if (b.kind != BLOCK_TOOL_USE) return;
    char *name = tool_name(json_get(start, "name"));
    emit("{'type':'tool_use_start','id':%R,'name':'%r'}", json_get(start, "id"), name);
    free(name);
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
        span thinking = escaped_string_body(json_get(delta, "thinking"));
        buf_putspan(&b->escaped_text, thinking);
        if (thinking.n) emit("{'type':'thinking_delta','text':%R}", json_get(delta, "thinking"));
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

static void on_stream_event(json event)
{
    json type = json_get(event, "type");
    if (json_eq(type, "message_start")) reset_message(), read_usage(json_get(json_get(event, "message"), "usage"));
    else if (json_eq(type, "content_block_start")) start_block(event);
    else if (json_eq(type, "content_block_delta")) apply_delta(event);
    else if (json_eq(type, "message_delta")) finish_message(event);
    else if (json_eq(type, "message_stop")) st.saw_message_stop = true;
}

static void on_claude_line(json line)
{
    if (!json_valid(line)) return;
    json type = json_get(line, "type");
    if (json_eq(type, "stream_event") && json_null_or_absent(json_get(line, "parent_tool_use_id")))
        on_stream_event(json_get(line, "event"));
    else if (json_eq(type, "result"))
        buf_set(&st.final_result, line), st.results++;
}

static void consume_lines(void)
{
    buf *pending = &st.pending;
    char *nl;
    while (pending->n && (nl = memchr(pending->p, '\n', pending->n))) {
        on_claude_line(json_trim((span){ pending->p, (size_t)(nl - pending->p) }));
        buf_drop_front(pending, (size_t)(nl - pending->p) + 1);
    }
}

static bool read_claude(int fd)
{
    char chunk[65536];
    ssize_t n = read(fd, chunk, sizeof chunk);
    if (n < 0) return errno == EINTR || errno == EAGAIN;
    buf_put(&st.pending, chunk, (size_t)n);
    consume_lines();
    return n > 0;
}

static void write_claude(int *fd, span input, size_t *written, size_t *pauses_passed)
{
    size_t until = *pauses_passed < st.npauses ? st.pauses[*pauses_passed] : input.n;
    ssize_t n = write(*fd, input.p + *written, until - *written);
    if (n < 0 && errno != EAGAIN && errno != EINTR) until = *written = input.n;
    if (n > 0) *written += (size_t)n;
    if (*written == until && *pauses_passed < st.npauses) ++*pauses_passed;
    if (*written == input.n) close(*fd), *fd = -1;
}

static void converse(int in, int out, span input)
{
    size_t written = 0, pauses_passed = 0;
    while (!st.terminated) {
        bool awaiting_ack = pauses_passed > st.results;
        struct pollfd fds[2] = { { out, POLLIN, 0 }, { awaiting_ack ? -1 : in, POLLOUT, 0 } };
        int ready = poll(fds, 2, awaiting_ack ? ACK_TIMEOUT_MS : -1);
        if (ready == 0) {
            st.failure = "claude did not take in the conversation";
            break;
        }
        if (ready < 0 && errno != EINTR) break;
        if (ready < 0) continue;
        if (fds[1].revents) write_claude(&in, input, &written, &pauses_passed);
        if (fds[0].revents && !read_claude(out)) break;
    }
    if (in >= 0) close(in);
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
    switch (b->kind) {
    case BLOCK_TEXT:     return b->escaped_text.n > 0;
    case BLOCK_THINKING: return b->escaped_signature.n > 0;
    case BLOCK_REDACTED: return true;
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
    if (b->kind == BLOCK_TEXT) {
        json_printf(out, "{'type':'text','text':'%r'}", buf_cstr(&b->escaped_text));
    } else if (b->kind == BLOCK_THINKING) {
        json_printf(out, "{'type':'thinking','thinking':'%r','signature':'%r'}",
                    buf_cstr(&b->escaped_text), buf_cstr(&b->escaped_signature));
    } else if (b->kind == BLOCK_TOOL_USE) {
        char *name = tool_name(json_get(start, "name"));
        json_printf(out, "{'type':'tool_use','id':%R,'name':'%r','input':", json_get(start, "id"), name);
        put_tool_input(out, b);
        buf_putc(out, '}');
        free(name);
    } else {
        buf_putspan(out, start);
    }
}

static int emit_result(outcome o)
{
    json_buf line = { 0 };
    json_printf(&line, "{'type':'result','stop':%s,'message':{'role':'assistant','content':[", stop_name(o));
    for (size_t i = 0; i < st.nblocks; i++) {
        if (!keep_block(&st.blocks[i], o)) continue;
        comma_unless_first(&line);
        put_block(&line, &st.blocks[i]);
    }
    json_printf(&line, "]},'usage':{'input':%D,'output':%D,'cache_read':%D,'cache_write':%D}}",
                st.usage_tokens[USAGE_INPUT], st.usage_tokens[USAGE_OUTPUT],
                st.usage_tokens[USAGE_CACHE_READ], st.usage_tokens[USAGE_CACHE_WRITE]);
    print_and_free_line(&line);
    return 0;
}

static bool mentions_overflow(const char *message)
{
    return strstr(message, "rompt is too long") || strstr(message, "context limit") || strstr(message, "context window");
}

static const char *error_kind(const char *message)
{
    long s = st.api_status;
    if (s == 401 || s == 403 || strstr(message, "/login")) return "auth";
    if (s == 429) return "rate_limit";
    if (s == 529) return "overloaded";
    if (s == 413 || mentions_overflow(message)) return "context_overflow";
    if (s >= 400 && s < 500) return "invalid_request";
    return "other";
}

static int emit_claude_failure(int status)
{
    json result = buf_span(&st.final_result), text = json_get(result, "result");
    jiter errors = json_iter(json_get(result, "errors"));
    json first_error;
    buf message = { 0 };
    if (st.failure) buf_puts(&message, st.failure);
    else if (json_is(text, '"')) json_text(text, &message);
    else if (json_next(&errors, NULL, &first_error)) json_text(first_error, &message);
    if (!message.n && WIFSIGNALED(status)) buf_printf(&message, "claude was killed by signal %d", WTERMSIG(status));
    else if (!message.n) buf_printf(&message, "claude exited with status %d without a response", WEXITSTATUS(status));
    st.api_status = (long)json_num(json_get(result, "api_error_status"), 0);
    return emit_error(error_kind(buf_cstr(&message)), buf_cstr(&message));
}

static int run(char **args, json params, span input)
{
    int in, out;
    pid_t pid = start_claude(args, params, &in, &out);
    if (pid < 0) return emit_error("other", strerror(errno));
    converse(in, out, input);
    if (st.terminated || st.failure) kill(pid, SIGTERM);
    if (st.terminated) return emit_result(ABORTED);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR && !st.terminated) {}
    if (st.terminated) return emit_result(ABORTED);
    return st.saw_message_stop ? emit_result(COMPLETE) : emit_claude_failure(status);
}

static void answer_mcp(json request, const char *tools)
{
    json id = json_get(request, "id"), method = json_get(request, "method");
    if (!id.p) return;
    json_buf reply = { 0 };
    json_printf(&reply, "{'jsonrpc':'2.0','id':%R,", id);
    if (json_eq(method, "initialize"))
        json_printf(&reply, "'result':{'protocolVersion':%R,'capabilities':{'tools':{}},'serverInfo':{'name':'akern','version':'1'}}}",
                    json_get(json_get(request, "params"), "protocolVersion"));
    else if (json_eq(method, "tools/list"))
        json_printf(&reply, "'result':{'tools':%r}}", tools);
    else if (json_eq(method, "tools/call"))
        json_printf(&reply, "'result':{'isError':true,'content':[{'type':'text','text':'Not run: akern runs its tools itself.'}]}}");
    else if (json_eq(method, "ping"))
        json_printf(&reply, "'result':{}}");
    else
        json_printf(&reply, "'error':{'code':-32601,'message':'Method not found'}}");
    print_and_free_line(&reply);
}

static int serve_tools(const char *tools)
{
    char *line = NULL;
    size_t cap = 0;
    for (ssize_t n; (n = getline(&line, &cap, stdin)) > 0;)
        if (json_valid((span){ line, (size_t)n })) answer_mcp(json_trim((span){ line, (size_t)n }), tools);
    free(line);
    return 0;
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
    if (argc > 2 && !strcmp(argv[1], "--mcp")) return serve_tools(argv[2]);
    trap_sigterm();
    signal(SIGPIPE, SIG_IGN);
    json_buf request = { 0 }, input = { 0 };
    strv args = { 0 };
    read_all(stdin, &request);
    if (!json_valid(buf_span(&request)) || !build_invocation(buf_span(&request), argv[0], &args, &input))
        return emit_error("invalid_request", "expected {\"model\",\"messages\",...} on stdin");
    return run(args.v, json_get(buf_span(&request), "params"), buf_span(&input));
}
