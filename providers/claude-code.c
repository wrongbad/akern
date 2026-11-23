#include "anthropic_messages.h"
#include "provider.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum { ACK_TIMEOUT_MS = 60000 };

static const char TOOL_PREFIX[] = "mcp__akern__";

static const char *const CLAUDE_ARGS[] = {
    "claude",
    "-p",
    "--input-format",
    "stream-json",
    "--output-format",
    "stream-json",
    "--verbose",
    "--include-partial-messages",
    "--no-session-persistence",
    "--tools",
    "",
    "--strict-mcp-config",
    "--setting-sources",
    "",
    "--disable-slash-commands",
    "--permission-mode",
    "dontAsk",
    "--mcp-config",
    "{\"mcpServers\":{\"akern\":{\"type\":\"sdk\",\"name\":\"akern\"}}}",
};

static const char *const CLAUDE_ALIASES[] = {"fable", "sonnet", "opus", "haiku"};

/* A call waits for akern to run it, which can take as long as a job's grace; a result is
   already as long as akern lets it be. */
static const char *const CLAUDE_ENV[][2] = {
    {"CLAUDE_CODE_DISABLE_CLAUDE_MDS", "1"},
    {"CLAUDE_CODE_DISABLE_AUTO_MEMORY", "1"},
    {"CLAUDE_CODE_DISABLE_ADVISOR_TOOL", "1"},
    {"DISABLE_COMPACT", "1"},
    {"MCP_TOOL_TIMEOUT", "2147483647"},
    {"MAX_MCP_OUTPUT_TOKENS", "1000000000"},
};

static const char *const KEPT_CLAUDE_ENV[] = {
    "CLAUDE_CONFIG_DIR",
    "CLAUDE_CODE_OAUTH_TOKEN",
    "CLAUDE_CODE_USE_BEDROCK",
    "CLAUDE_CODE_USE_VERTEX",
    "CLAUDE_CODE_USE_FOUNDRY",
};

/* Where claude is between requests. ENDING: the model ended its turn and claude's result line,
   which closes it, may still be on its way. CALLING: the model called tools, and claude asks for
   each call's result over MCP; the next request carries them. */
typedef enum { NONE, IDLE, ENDING, CALLING } phase;

static struct {
    pid_t pid;
    int in, out;
    phase phase;
    char *setup;            /* the request fields claude was started with */
    json_buf fed;           /* the rendered messages claude holds, as a JSON array */
    json_buf mcp_tools;     /* the tools as MCP lists them */
    buf from_claude;        /* stdout not yet a whole line */
    buf writing, controls;  /* stdin bytes in flight, and control responses to send next */
    buf input;              /* history lines, sent up to the next unacknowledged pause */
    size_t *pauses;         /* offsets in input just past each user line claude queues */
    size_t npauses, cappauses, input_sent;
    size_t results;         /* claude's result lines so far */
    size_t results_at_feed; /* ... when the input was queued */
    size_t results_at_stop; /* ... when the reply's message_stop arrived */
    bool streaming;         /* reading the reply's stream events */
    bool settling;          /* waiting, a bounded time, for the result that closes a turn */
    json_buf last_result;
    strv held;              /* tools/call control requests waiting for a result */
    json_buf answers;       /* the tool_result blocks of the request that answers the calls */
    strv call_ids;          /* the tool_use ids of the latest reply */
    const char *failure;
} cc;

/* ---- the request ---- */

static void put_setup(json_buf *out, json request)
{
    json params = json_get(request, "params");
    json_printf(
        out,
        "{'model':%R,'system':%R,'tools':%R,'effort':%R,'max_tokens':%R}",
        json_get(request, "model"),
        json_get(request, "system"),
        json_get(request, "tools"),
        json_get(params, "effort"),
        json_get(params, "max_tokens")
    );
}

static void put_mcp_tools(json_buf *out, json tools)
{
    jiter it = json_iter(tools);
    buf_putc(out, '[');
    for (json tool; json_next(&it, NULL, &tool);)
    {
        json_put_comma_unless_first(out);
        json_printf(
            out,
            "{'name':%R,'description':%R,'inputSchema':%R}",
            json_get(tool, "name"),
            json_get(tool, "description"),
            json_get(tool, "input_schema")
        );
    }
    buf_putc(out, ']');
}

/* mcp__akern__a,mcp__akern__b: the calls claude may make without asking. */
static char *allowed_tools(json tools)
{
    buf names = {0};
    jiter it = json_iter(tools);
    for (json tool; json_next(&it, NULL, &tool);)
    {
        if (names.n)
            buf_putc(&names, ',');
        buf_puts(&names, TOOL_PREFIX);
        json_decode(json_get(tool, "name"), &names);
    }
    return buf_cstr(&names);
}

/* ---- the rendered history ---- */

static size_t count_items(json array)
{
    size_t n = 0;
    jiter it = json_iter(array);
    for (json v; json_next(&it, NULL, &v);)
        n++;
    return n;
}

/* How many of rendered's messages are fed's, all of fed in order; -1 when fed is not a prefix. */
static long shared_prefix(json fed, json rendered)
{
    jiter a = json_iter(fed), b = json_iter(rendered);
    long n = 0;
    for (json x, y; json_next(&a, NULL, &x); n++)
    {
        if (!json_next(&b, NULL, &y))
            return -1;
        x = json_trim(x), y = json_trim(y);
        if (x.n != y.n || memcmp(x.p, y.p, x.n))
            return -1;
    }
    return n;
}

/* The items of array from index from on, as a JSON array. */
static void put_items_from(json_buf *out, json array, size_t from, size_t to)
{
    jiter it = json_iter(array);
    size_t i = 0;
    buf_putc(out, '[');
    for (json v; json_next(&it, NULL, &v); i++)
        if (i >= from && i < to)
        {
            json_put_comma_unless_first(out);
            buf_putspan(out, json_trim(v));
        }
    buf_putc(out, ']');
}

static bool is_tool_result(json block)
{
    return json_eq(json_get(block, "type"), "tool_result");
}

/* ---- talking to claude ---- */

static void send_control(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    json_vprintf(&cc.controls, fmt, ap);
    va_end(ap);
    buf_putc(&cc.controls, '\n');
}

static void reply_mcp(json request_id, json message_id, const char *result_fmt, json value)
{
    json_buf result = {0};
    json_printf(&result, result_fmt, value);
    send_control(
        "{'type':'control_response','response':{'subtype':'success','request_id':%R,"
        "'response':{'mcp_response':{'jsonrpc':'2.0','id':%R,'result':%R}}}}",
        request_id,
        message_id.p ? message_id : S("0"),
        buf_span(&result)
    );
    buf_free(&result);
}

/* A rendered tool_result as an MCP tools/call result; false for a block MCP cannot carry. */
static bool put_call_result(json_buf *out, json tool_result)
{
    json content = json_get(tool_result, "content");
    json_printf(out, "{'isError':%b,'content':[", json_is_true(json_get(tool_result, "is_error")));
    if (json_is(content, '"'))
        json_printf(out, "{'type':'text','text':%R}", content);
    jiter it = json_iter(json_is(content, '[') ? content : NOSPAN);
    bool first = true;
    for (json block; json_next(&it, NULL, &block); first = false)
    {
        json type = json_get(block, "type"), source = json_get(block, "source");
        if (!first)
            buf_putc(out, ',');
        if (json_eq(type, "text"))
            json_printf(out, "{'type':'text','text':%R}", json_get(block, "text"));
        else if (json_eq(type, "image") && json_eq(json_get(source, "type"), "base64"))
            json_printf(
                out,
                "{'type':'image','data':%R,'mimeType':%R}",
                json_get(source, "data"),
                json_get(source, "media_type")
            );
        else
            return false;
    }
    buf_puts(out, "]}");
    return true;
}

static bool same_string(json a, json b)
{
    span x = json_get_escaped_body(a), y = json_get_escaped_body(b);
    return json_is(a, '"') && json_is(b, '"') && x.n == y.n && !memcmp(x.p, y.p, x.n);
}

/* The tool_use id claude puts in a tools/call's _meta. */
static json call_id(json call)
{
    return json_get(
        json_get(json_get(json_get(call, "message"), "params"), "_meta"),
        "claudecode/toolUseId"
    );
}

/* Answers a tools/call from the request's results; false when they do not have it yet. */
static bool answer_call(json control)
{
    json id = call_id(json_get(control, "request"));
    jiter it = json_iter(buf_span(&cc.answers));
    for (json block; json_next(&it, NULL, &block);)
    {
        if (!same_string(json_get(block, "tool_use_id"), id))
            continue;
        json_buf result = {0};
        put_call_result(&result, block);
        reply_mcp(
            json_get(control, "request_id"),
            json_get(json_get(json_get(control, "request"), "message"), "id"),
            "%R",
            buf_span(&result)
        );
        buf_free(&result);
        return true;
    }
    return false;
}

static void answer_held_calls(void)
{
    strv still = {0};
    for (size_t i = 0; i < cc.held.n; i++)
        if (!answer_call(S(cc.held.v[i])))
            strv_push(&still, xstrdup(cc.held.v[i]));
    strv_free(&cc.held);
    cc.held = still;
}

static void on_mcp(json control)
{
    json request_id = json_get(control, "request_id");
    json message = json_get(json_get(control, "request"), "message");
    json method = json_get(message, "method"), id = json_get(message, "id");
    if (json_eq(method, "initialize"))
        reply_mcp(
            request_id,
            id,
            "{'protocolVersion':%R,'capabilities':{'tools':{}},'serverInfo':{'name':'akern',"
            "'version':'1'}}",
            json_get(json_get(message, "params"), "protocolVersion")
        );
    else if (json_eq(method, "tools/list"))
        reply_mcp(request_id, id, "{'tools':%R}", buf_span(&cc.mcp_tools));
    else if (json_eq(method, "tools/call"))
    {
        if (!answer_call(control))
            strv_push(&cc.held, span_dup(control));
    }
    else if (!id.p || json_eq(method, "ping"))
        reply_mcp(request_id, id, "{}", NOSPAN);
    else
        send_control(
            "{'type':'control_response','response':{'subtype':'success','request_id':%R,"
            "'response':{'mcp_response':{'jsonrpc':'2.0','id':%R,'error':{'code':-32601,"
            "'message':'Method not found'}}}}}",
            request_id,
            id
        );
}

static void on_claude_line(json line)
{
    if (!json_is_valid(line))
        return;
    json type = json_get(line, "type");
    if (json_eq(type, "control_request"))
    {
        json request = json_get(line, "request");
        if (json_eq(json_get(request, "subtype"), "mcp_message"))
            on_mcp(line);
        else
            send_control(
                "{'type':'control_response','response':{'subtype':'error','request_id':%R,"
                "'error':'akern answers MCP messages only'}}",
                json_get(line, "request_id")
            );
    }
    else if (json_eq(type, "stream_event") && cc.streaming &&
             json_is_null_or_absent(json_get(line, "parent_tool_use_id")))
    {
        anthropic_read_event(json_get(line, "event"));
        if (anthropic_is_complete())
            cc.streaming = false, cc.results_at_stop = cc.results;
    }
    else if (json_eq(type, "result"))
    {
        buf_set(&cc.last_result, line);
        cc.results++;
    }
}

static void consume_lines(void)
{
    buf *pending = &cc.from_claude;
    char *nl;
    while (pending->n && (nl = memchr(pending->p, '\n', pending->n)))
    {
        on_claude_line(json_trim((span){pending->p, (size_t)(nl - pending->p)}));
        buf_drop_front(pending, (size_t)(nl - pending->p) + 1);
    }
}

static bool read_claude(void)
{
    char chunk[65536];
    ssize_t n = read(cc.out, chunk, sizeof chunk);
    if (n < 0)
        return errno == EINTR || errno == EAGAIN;
    buf_put(&cc.from_claude, chunk, (size_t)n);
    consume_lines();
    return n > 0;
}

/* claude queues a user line that asks for no reply and acknowledges it with a result line;
   the input goes no further than the first pause not yet acknowledged. */
static size_t input_allowed(void)
{
    size_t acks = cc.results - cc.results_at_feed;
    return acks < cc.npauses ? cc.pauses[acks] : cc.input.n;
}

static bool awaiting_ack(void)
{
    return !cc.writing.n && !cc.controls.n && cc.input_sent == input_allowed() &&
           cc.input_sent < cc.input.n;
}

/* Whole lines only, control responses first. */
static void fill_writing(void)
{
    if (cc.writing.n)
        return;
    if (cc.controls.n)
    {
        buf_putspan(&cc.writing, buf_span(&cc.controls));
        cc.controls.n = 0;
        return;
    }
    size_t until = input_allowed();
    if (until > cc.input_sent)
    {
        buf_put(&cc.writing, cc.input.p + cc.input_sent, until - cc.input_sent);
        cc.input_sent = until;
    }
}

static void write_claude(void)
{
    ssize_t n = write(cc.in, cc.writing.p, cc.writing.n);
    if (n > 0)
        buf_drop_front(&cc.writing, (size_t)n);
    else if (n < 0 && errno != EAGAIN && errno != EINTR)
        cc.writing.n = 0; /* claude is gone: its stdout ends next */
}

/* Talks to claude until done() holds; false if claude ended, the wait for an acknowledgement
   ran out, or SIGTERM arrived. */
static bool pump(bool (*done)(void))
{
    while (!done())
    {
        if (provider_was_aborted())
            return false;
        fill_writing();
        struct pollfd fds[2] = {{cc.out, POLLIN, 0}, {cc.writing.n ? cc.in : -1, POLLOUT, 0}};
        int ready = poll(fds, 2, awaiting_ack() || cc.settling ? ACK_TIMEOUT_MS : -1);
        if (ready == 0)
        {
            cc.failure = "claude did not take in the conversation";
            return false;
        }
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            return false;
        if (fds[1].revents)
            write_claude();
        if (fds[0].revents && !read_claude())
            return false;
    }
    return true;
}

/* ---- claude's life ---- */

static void stop_claude(void)
{
    if (cc.pid > 0)
    {
        close(cc.in), close(cc.out);
        kill(cc.pid, SIGTERM);
        while (waitpid(cc.pid, NULL, 0) < 0 && errno == EINTR)
        {
        }
    }
    cc.pid = 0;
    cc.phase = NONE;
    free(cc.setup), cc.setup = NULL;
    cc.fed.n = cc.from_claude.n = cc.writing.n = cc.controls.n = cc.input.n = 0;
    cc.npauses = cc.input_sent = cc.results = cc.results_at_feed = cc.results_at_stop = 0;
    cc.streaming = false;
    strv_free(&cc.held);
    strv_free(&cc.call_ids);
    cc.answers.n = 0;
}

static bool claude_is_alive(void)
{
    return cc.pid > 0 && waitpid(cc.pid, NULL, WNOHANG) == 0;
}

static bool host_session_variable(span name)
{
    if (name.n < 6 || memcmp(name.p, "CLAUDE", 6))
        return false;
    for (size_t i = 0; i < LEN(KEPT_CLAUDE_ENV); i++)
        if (span_eq(name, KEPT_CLAUDE_ENV[i]))
            return false;
    return true;
}

static void set_claude_env(json params)
{
    extern char **environ;
    strv names = {0};
    for (char **e = environ; *e; e++)
    {
        span name = {*e, strcspn(*e, "=")};
        if (host_session_variable(name))
            strv_push(&names, span_dup(name));
    }
    for (size_t i = 0; i < names.n; i++)
        unsetenv(names.v[i]);
    for (size_t i = 0; i < LEN(CLAUDE_ENV); i++)
        setenv(CLAUDE_ENV[i][0], CLAUDE_ENV[i][1], 1);
    json max_tokens = json_get(params, "max_tokens");
    if (json_get_num(max_tokens, 0) >= 1)
    {
        char text[32];
        snprintf(text, sizeof text, "%.0f", json_get_num(max_tokens, 0));
        setenv("CLAUDE_CODE_MAX_OUTPUT_TOKENS", text, 1);
    }
}

static void put_args(strv *args, json request)
{
    json system = json_get(request, "system");
    json effort = json_get(json_get(request, "params"), "effort");
    for (size_t i = 0; i < LEN(CLAUDE_ARGS); i++)
        strv_push(args, xstrdup(CLAUDE_ARGS[i]));
    strv_push(args, xstrdup("--allowedTools"));
    strv_push(args, allowed_tools(json_get(request, "tools")));
    strv_push(args, xstrdup("--model"));
    strv_push(args, json_decode_dup(json_get(request, "model")));
    strv_push(args, xstrdup("--system-prompt"));
    strv_push(args, json_is(system, '"') ? json_decode_dup(system) : xstrdup(""));
    if (json_is(effort, '"'))
        strv_push(args, xstrdup("--effort")), strv_push(args, json_decode_dup(effort));
    strv_push(args, NULL);
}

static bool start_claude(json request)
{
    strv args = {0};
    put_args(&args, request);
    int in[2], out[2];
    if (pipe(in) || pipe(out))
        provider_emit_error("other", strerror(errno), 0);
    pid_t pid = fork();
    if (pid == 0)
    {
        dup2(in[0], 0), dup2(out[1], 1);
        close(in[0]), close(in[1]), close(out[0]), close(out[1]);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        set_claude_env(json_get(request, "params"));
        execvp(args.v[0], args.v);
        dprintf(2, "%s: %s\n", args.v[0], strerror(errno));
        _exit(127);
    }
    strv_free(&args);
    close(in[0]), close(out[1]);
    if (pid < 0)
    {
        close(in[1]), close(out[0]);
        return false;
    }
    fcntl(in[1], F_SETFL, O_NONBLOCK);
    fcntl(in[1], F_SETFD, FD_CLOEXEC);
    fcntl(out[0], F_SETFD, FD_CLOEXEC);
    cc.pid = pid, cc.in = in[1], cc.out = out[0];
    cc.phase = IDLE;
    cc.mcp_tools.n = 0;
    put_mcp_tools(&cc.mcp_tools, json_get(request, "tools"));
    send_control(
        "{'type':'control_request','request_id':'akern-init','request':{'subtype':'initialize',"
        "'sdkMcpServers':['akern']}}"
    );
    return true;
}

/* Queues rendered messages as stream-json lines: every user message but the last one asks for
   no reply, and an assistant message is appended as it is read. */
static void queue_messages(json messages)
{
    jiter it = json_iter(messages);
    json message, next;
    cc.input.n = cc.npauses = cc.input_sent = 0;
    cc.results_at_feed = cc.results;
    for (bool more = json_next(&it, NULL, &message); more; message = next)
    {
        more = json_next(&it, NULL, &next);
        if (json_eq(json_get(message, "role"), "user"))
        {
            json_printf(
                &cc.input,
                "{'type':'user',%r'message':%R}\n",
                more ? "\"shouldQuery\":false," : "",
                message
            );
            if (more)
                VEC_PUSH(cc.pauses, cc.npauses, cc.cappauses, cc.input.n);
            continue;
        }
        json_printf(&cc.input, "{'type':'assistant','message':%R}\n", message);
    }
}

/* ---- one request ---- */

static bool turn_closed(void)
{
    return cc.results > cc.results_at_stop;
}

static long call_index(json tool_use_id)
{
    for (size_t i = 0; i < cc.call_ids.n; i++)
        if (json_eq(tool_use_id, cc.call_ids.v[i]))
            return (long)i;
    return -1;
}

/* The next request carries one result for each call of the reply, and nothing else: claude
   gets them as the answers to its tools/call requests. */
static bool continue_calls(json tail)
{
    json message = NOSPAN;
    jiter it = json_iter(tail);
    if (count_items(tail) != 1 || !json_next(&it, NULL, &message) ||
        !json_eq(json_get(message, "role"), "user") || cc.call_ids.n > 64)
        return false;
    json content = json_get(message, "content");
    jiter blocks = json_iter(content);
    bool answered[64] = {false};
    size_t n = 0;
    for (json block; json_next(&blocks, NULL, &block); n++)
    {
        json_buf scratch = {0};
        long i = is_tool_result(block) ? call_index(json_get(block, "tool_use_id")) : -1;
        bool fits = i >= 0 && !answered[i] && put_call_result(&scratch, block);
        buf_free(&scratch);
        if (!fits)
            return false;
        answered[i] = true;
    }
    if (n != cc.call_ids.n)
        return false;
    buf_set(&cc.answers, json_trim(content));
    cc.input.n = cc.npauses = cc.input_sent = 0;
    cc.results_at_feed = cc.results;
    answer_held_calls();
    return true;
}

/* Feeds claude only what is new since its last reply, when what it holds is where the request
   starts and the new part is something it can take mid-conversation. */
static bool continue_claude(json rendered, const char *setup)
{
    if (!cc.setup || strcmp(cc.setup, setup) || !claude_is_alive())
        return false;
    long shared = shared_prefix(buf_span(&cc.fed), rendered);
    size_t total = count_items(rendered);
    if (shared < 0 || (size_t)shared >= total)
        return false;
    json_buf tail = {0};
    put_items_from(&tail, rendered, (size_t)shared, total);
    bool ok = false;
    cc.settling = true;
    if (cc.phase == ENDING && pump(turn_closed))
        cc.phase = IDLE;
    cc.settling = false;
    cc.failure = NULL;
    if (cc.phase == CALLING)
        ok = continue_calls(buf_span(&tail));
    else if (cc.phase == IDLE)
    {
        jiter it = json_iter(buf_span(&tail));
        json first = NOSPAN, block = NOSPAN;
        json_next(&it, NULL, &first);
        jiter blocks = json_iter(json_get(first, "content"));
        bool answers_calls = json_next(&blocks, NULL, &block) && is_tool_result(block);
        if (!answers_calls)
            queue_messages(buf_span(&tail)), ok = true;
    }
    buf_free(&tail);
    return ok;
}

static void fail(void)
{
    json result = buf_span(&cc.last_result), text = json_get(result, "result");
    jiter errors = json_iter(json_get(result, "errors"));
    json first_error;
    buf message = {0};
    int status = 0;
    if (cc.pid > 0 && waitpid(cc.pid, &status, WNOHANG) <= 0)
        status = 0;
    bool exited = cc.pid > 0 && status != 0;
    if (cc.failure)
        buf_puts(&message, cc.failure);
    else if (json_is(text, '"'))
        json_decode(text, &message);
    else if (json_next(&errors, NULL, &first_error))
        json_decode(first_error, &message);
    if (!message.n && exited && WIFSIGNALED(status))
        buf_printf(&message, "claude was killed by signal %d", WTERMSIG(status));
    else if (!message.n && exited)
        buf_printf(&message, "claude exited with status %d without a response", WEXITSTATUS(status));
    else if (!message.n)
        buf_puts(&message, "claude ended without a response");
    long api_status = (long)json_get_num(json_get(result, "api_error_status"), 0);
    const char *msg = buf_cstr(&message), *kind = "other";
    if (api_status == 401 || api_status == 403 || strstr(msg, "/login"))
        kind = "auth";
    else if (api_status == 429)
        kind = "rate_limit";
    else if (api_status == 529)
        kind = "overloaded";
    else if (api_status == 413 || strstr(msg, "rompt is too long") ||
             strstr(msg, "context limit") || strstr(msg, "context window"))
        kind = "context_overflow";
    else if (api_status >= 400 && api_status < 500)
        kind = "invalid_request";
    stop_claude();
    provider_emit_error(kind, msg, api_status);
}

static bool reply_done(void)
{
    return anthropic_is_complete() || cc.results > cc.results_at_feed + cc.npauses;
}

/* What claude holds after the reply: the request's messages and the reply, rendered, without
   the placeholder answer the renderer adds after open calls. */
static void remember_reply(json messages, json content, bool calls)
{
    json_buf canonical = {0}, rendered = {0};
    json body = json_trim(messages);
    buf_put(&canonical, body.p, body.n - 1);
    json_printf(
        &canonical,
        "%r{'role':'assistant','content':%R}]",
        count_items(body) ? "," : "",
        content
    );
    anthropic_render(&rendered, buf_span(&canonical));
    size_t n = count_items(buf_span(&rendered));
    cc.fed.n = 0;
    put_items_from(&cc.fed, buf_span(&rendered), 0, calls && n ? n - 1 : n);
    buf_free(&canonical);
    buf_free(&rendered);
}

static void note_calls(json content)
{
    strv_free(&cc.call_ids);
    jiter it = json_iter(content);
    for (json block; json_next(&it, NULL, &block);)
        if (json_eq(json_get(block, "type"), "tool_use"))
            strv_push(&cc.call_ids, json_decode_dup(json_get(block, "id")));
}

static void answer(json request)
{
    json_buf setup = {0}, rendered = {0};
    put_setup(&setup, request);
    anthropic_name_tools(TOOL_PREFIX);
    anthropic_render(&rendered, json_get(request, "messages"));
    cc.failure = NULL;
    if (!continue_claude(buf_span(&rendered), buf_cstr(&setup)))
    {
        stop_claude();
        if (!start_claude(request))
            provider_emit_error("other", strerror(errno), 0);
        cc.setup = xstrdup(buf_cstr(&setup));
        queue_messages(buf_span(&rendered));
    }
    anthropic_begin_stream();
    cc.streaming = true;
    pump(reply_done);
    cc.streaming = false;
    if (provider_was_aborted())
    {
        stop_claude();
        anthropic_emit_result(true);
        exit(0);
    }
    if (!anthropic_is_complete())
        fail();
    json_buf content = {0};
    anthropic_put_content(&content, false);
    const char *stop = anthropic_stop();
    bool calls = !strcmp(stop, "tool_use");
    remember_reply(json_get(request, "messages"), buf_span(&content), calls);
    cc.answers.n = 0;
    if (calls)
        cc.phase = CALLING, note_calls(buf_span(&content));
    else if (!strcmp(stop, "end_turn"))
        cc.phase = ENDING;
    else
        stop_claude(); /* after max_tokens claude goes on by itself */
    anthropic_emit_result(false);
    buf_free(&content);
    buf_free(&setup);
    buf_free(&rendered);
}

static bool on_path(const char *name)
{
    char file[4096];
    for (const char *dir = getenv("PATH"); dir;)
    {
        int n = (int)strcspn(dir, ":");
        snprintf(file, sizeof file, "%.*s/%s", n ? n : 1, n ? dir : ".", name);
        if (access(file, X_OK) == 0)
            return true;
        dir = dir[n] ? dir + n + 1 : NULL;
    }
    return false;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--models"))
    {
        if (!on_path(CLAUDE_ARGS[0]))
            provider_emit_error("other", "claude is not on PATH", 0);
        for (size_t i = 0; i < LEN(CLAUDE_ALIASES); i++)
            provider_emit_model(CLAUDE_ALIASES[i], 0, 0);
        return 0;
    }
    signal(SIGPIPE, SIG_IGN);
    provider_stay();
    for (json request; provider_next_request(&request);)
        answer(request);
    stop_claude();
    return 0;
}
