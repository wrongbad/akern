#include "lib/messages.h"
#include "lib/provider.h"

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

enum { ACK_TIMEOUT_MS = 60000 };

static const char TOOL_PREFIX[] = "mcp__akern__";

static const char *const CLAUDE_ARGS[] = {
    "claude", "-p", "--input-format", "stream-json", "--output-format", "stream-json", "--verbose",
    "--include-partial-messages", "--no-session-persistence", "--tools", "", "--strict-mcp-config",
    "--setting-sources", "", "--disable-slash-commands", "--permission-mode", "dontAsk", "--max-turns", "1",
};

static const char *const CLAUDE_ALIASES[] = { "fable", "sonnet", "opus", "haiku" };

static const char *const CLAUDE_ENV[][2] = {
    { "CLAUDE_CODE_DISABLE_CLAUDE_MDS", "1" },
    { "CLAUDE_CODE_DISABLE_AUTO_MEMORY", "1" },
    { "CLAUDE_CODE_DISABLE_ADVISOR_TOOL", "1" },
    { "DISABLE_COMPACT", "1" },
};

static const char *const KEPT_CLAUDE_ENV[] = {
    "CLAUDE_CONFIG_DIR", "CLAUDE_CODE_OAUTH_TOKEN", "CLAUDE_CODE_USE_BEDROCK", "CLAUDE_CODE_USE_VERTEX", "CLAUDE_CODE_USE_FOUNDRY",
};

static struct {
    buf         pending;
    json_buf    final_result;
    size_t     *pauses;
    size_t      npauses, cappauses, results;
    const char *failure;
} st;

static void put_input_lines(buf *lines, json messages)
{
    jiter it = json_iter(messages);
    json message, next;
    for (bool more = json_next(&it, NULL, &message); more; message = next) {
        more = json_next(&it, NULL, &next);
        if (json_eq(json_get(message, "role"), "user")) {
            json_printf(lines, "{'type':'user',%r'message':%R}\n", more ? "\"shouldQuery\":false," : "", message);
            if (more) VEC_PUSH(st.pauses, st.npauses, st.cappauses, lines->n);
            continue;
        }
        json_printf(lines, "{'type':'assistant','message':%R}\n", message);
    }
}

static void put_mcp_config(json_buf *out, const char *self, json tools)
{
    json_buf listed = { 0 };
    jiter it = json_iter(tools);
    buf_putc(&listed, '[');
    for (json tool; json_next(&it, NULL, &tool);) {
        json_comma_unless_first(&listed);
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

static bool on_path(const char *name)
{
    char file[PATH_MAX];
    for (const char *dir = getenv("PATH"); dir;) {
        int n = (int)strcspn(dir, ":");
        snprintf(file, sizeof file, "%.*s/%s", n ? n : 1, n ? dir : ".", name);
        if (access(file, X_OK) == 0) return true;
        dir = dir[n] ? dir + n + 1 : NULL;
    }
    return false;
}

static void build_invocation(json request, const char *argv0, strv *args, buf *input)
{
    json model_id = json_get(request, "model"), system = json_get(request, "system");
    json effort = json_get(json_get(request, "params"), "effort");
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
    json_buf rendered = { 0 };
    messages_name_tools(TOOL_PREFIX);
    messages_render(&rendered, json_get(request, "messages"));
    put_input_lines(input, buf_span(&rendered));
    free(self);
    buf_free(&config);
    buf_free(&rendered);
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

static void on_claude_line(json line)
{
    if (!json_valid(line)) return;
    json type = json_get(line, "type");
    if (json_eq(type, "stream_event") && json_null_or_absent(json_get(line, "parent_tool_use_id")))
        messages_event(json_get(line, "event"));
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
    while (!provider_aborted()) {
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

static bool mentions_overflow(const char *message)
{
    return strstr(message, "rompt is too long") || strstr(message, "context limit") || strstr(message, "context window");
}

static const char *error_kind(long s, const char *message)
{
    if (s == 401 || s == 403 || strstr(message, "/login")) return "auth";
    if (s == 429) return "rate_limit";
    if (s == 529) return "overloaded";
    if (s == 413 || mentions_overflow(message)) return "context_overflow";
    if (s >= 400 && s < 500) return "invalid_request";
    return "other";
}

static void fail(int status)
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
    long api_status = (long)json_num(json_get(result, "api_error_status"), 0);
    provider_error(error_kind(api_status, buf_cstr(&message)), buf_cstr(&message), api_status);
}

static void run(char **args, json params, span input)
{
    int in, out;
    pid_t pid = start_claude(args, params, &in, &out);
    if (pid < 0) provider_error("other", strerror(errno), 0);
    converse(in, out, input);
    if (provider_aborted() || st.failure) kill(pid, SIGTERM);
    if (provider_aborted()) messages_result(true);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR && !provider_aborted()) {}
    if (provider_aborted()) messages_result(true);
    if (messages_complete()) messages_result(false);
    fail(status);
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
    buf_putc(&reply, '\n');
    fwrite(reply.p, 1, reply.n, stdout);
    fflush(stdout);
    buf_free(&reply);
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

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--models")) {
        if (!on_path(CLAUDE_ARGS[0])) provider_error("other", "claude is not on PATH", 0);
        for (size_t i = 0; i < LEN(CLAUDE_ALIASES); i++) provider_model(CLAUDE_ALIASES[i], 0, 0);
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "--mcp")) return serve_tools(argv[2]);
    signal(SIGPIPE, SIG_IGN);
    json request = provider_request();
    json_buf input = { 0 };
    strv args = { 0 };
    build_invocation(request, argv[0], &args, &input);
    run(args.v, json_get(request, "params"), buf_span(&input));
}
