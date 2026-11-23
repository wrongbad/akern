/* akern-agent: a sub-agent that speaks ACP protocol 2 (acp-v2.md), run the way `akern -p` is: give
   it a task as an argument, read its answer on stdout, steer it with lines on stdin, continue it
   later with the same -s. Owns starting the agent, the handshake, creating or resuming its
   session, setting its config options, turning stdin lines into prompts and cancels, answering
   permission requests by policy, and deciding when the agent is done: idle, with nothing in the
   background. Knows nothing of models, tools or which agent it runs.

   The session file starts with a header line naming the ACP session, the agent command and the
   cwd, then holds every JSON-RPC line, {"in": …} from the agent and {"out": …} to it, so it can
   be read as it grows. The agent's stderr goes to FILE.stderr. */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "json.h"

/* How long the agent must stay idle with nothing in the background before it counts as done. An
   agent may report finished background work a moment before the run that result starts
   (acp-v2.md, Background work), and this covers that gap. */
static const double QUIET_SECONDS = 0.5;
/* How long the agent has to exit once its stdin is closed, then after SIGTERM. */
static const double CLOSE_SECONDS = 5, TERM_SECONDS = 2;
/* How much of the agent's stderr is repeated when it fails. */
enum { STDERR_TAIL_BYTES = 2000 };

enum { EXIT_OK = 0, EXIT_FAILED = 1, EXIT_USAGE = 2, EXIT_REFUSED = 3, EXIT_SIGNAL = 130 };

typedef enum { REQ_SETUP, REQ_PROMPT } request_kind;

typedef struct {
    long long id;
    request_kind kind;
} request;

/* An agent message of the current foreground work, by its messageId. */
typedef struct {
    char *id;
    buf text;
} message;

static struct {
    const char *agent, *cwd, *file;
    bool allow;
    strv options; /* KEY=VALUE, from -o and -m */

    pid_t pid;
    int to_agent, from_agent;
    buf from_partial;

    int log;
    bool log_ready; /* the header is written; lines before it are held */
    buf log_held;
    char *session_id;

    long long next_id;
    request *requests;
    size_t nrequests, caprequests;
    long long awaited; /* the setup request being waited for */
    buf response;      /* its response line, once it has come */
    bool responded;

    /* Done is decided from the order of what arrives: seq counts the agent's lines. */
    long long seq, sent_seq, answered_seq, busy_seq, state_seq;
    int prompts_open;
    long long inserted; /* prompts the agent took */
    bool idle, background_quiet, seen_state;
    double quiet_since; /* 0 while not quiet */
    char *stop;         /* the last idle state's stop reason */
    bool failed;        /* the last idle state was an error */
    bool fatal;         /* stop now: the first prompt was refused, or the agent is gone */
    int fatal_code;

    message *messages;
    size_t nmessages, capmessages;
    strv refused;

    bool stdin_open;
    buf in_partial;
} g = {.to_agent = -1, .from_agent = -1, .log = -1, .awaited = -1};

static volatile sig_atomic_t signalled;

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void on_signal(int sig)
{
    signalled = sig;
}

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("akern-agent: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* Like die, under this program's name: a usage or setup failure. */
static void quit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("akern-agent: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(EXIT_USAGE);
}

/* ---- the session file ---- */

static void log_line(const char *direction, span line)
{
    buf entry = {0};
    if (json_is_valid(line))
        json_printf(&entry, "{'%r':%R}\n", direction, line);
    else
        json_printf(&entry, "{'%r_text':%S}\n", direction, line);
    if (g.log_ready)
        write_all(g.log, buf_span(&entry));
    else
        buf_putspan(&g.log_held, buf_span(&entry));
    buf_free(&entry);
}

/* The header of a new session, then what was held until it was known. */
static void write_header(void)
{
    buf header = {0};
    json_printf(
        &header,
        "{'type':'acp_session','sessionId':%s,'agent':%s,'cwd':%s}\n",
        g.session_id,
        g.agent,
        g.cwd
    );
    buf_putspan(&header, buf_span(&g.log_held));
    write_all(g.log, buf_span(&header));
    buf_free(&header);
    buf_free(&g.log_held);
    g.log_ready = true;
}

/* The header of an existing session file: its first line. False when the file is empty. */
static bool read_header(void)
{
    buf first = {0};
    char chunk[4096];
    ssize_t n;
    while ((n = pread(g.log, chunk, sizeof chunk, (off_t)first.n)) > 0)
    {
        char *nl = memchr(chunk, '\n', (size_t)n);
        buf_put(&first, chunk, nl ? (size_t)(nl - chunk) : (size_t)n);
        if (nl)
            break;
    }
    if (!first.n)
        return false;
    json header = buf_span(&first);
    if (!json_is_valid(header) || !json_eq(json_get(header, "type"), "acp_session"))
        quit("%s is not an akern-agent session file", g.file);
    g.session_id = json_decode_dup(json_get(header, "sessionId"));
    char *agent = json_decode_dup(json_get(header, "agent"));
    char *cwd = json_decode_dup(json_get(header, "cwd"));
    if (!g.session_id)
        quit("%s names no session", g.file);
    if (!g.agent)
        g.agent = agent;
    else
        free(agent);
    if (!g.cwd)
        g.cwd = cwd;
    else
        free(cwd);
    buf_free(&first);
    return true;
}

/* Opens and locks the session file; true when it already holds a session to resume. */
static bool open_session_file(void)
{
    if (!g.file)
    {
        const char *tmp = getenv("TMPDIR");
        buf path = {0};
        buf_printf(&path, "%s/akern-agent-XXXXXX", tmp && *tmp ? tmp : "/tmp");
        char *p = buf_cstr(&path);
        g.log = mkstemp(p);
        if (g.log < 0)
            quit("cannot create a session file in %s: %s", tmp ? tmp : "/tmp", strerror(errno));
        g.file = p;
    }
    else if ((g.log = open(g.file, O_RDWR | O_CREAT | O_APPEND, 0644)) < 0)
        quit("cannot open %s: %s", g.file, strerror(errno));
    fcntl(g.log, F_SETFD, FD_CLOEXEC);
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    if (fcntl(g.log, F_SETLK, &lock) < 0)
        quit("%s is in use by another akern-agent", g.file);
    bool resuming = read_header();
    g.log_ready = resuming;
    return resuming;
}

/* ---- the agent process ---- */

static void start_agent(void)
{
    int in[2], out[2];
    if (pipe(in) < 0 || pipe(out) < 0)
        quit("pipe: %s", strerror(errno));
    buf err_path = {0};
    buf_printf(&err_path, "%s.stderr", g.file);
    int err = open(buf_cstr(&err_path), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (err < 0)
        quit("cannot open %s: %s", buf_cstr(&err_path), strerror(errno));
    buf_free(&err_path);
    g.pid = fork();
    if (g.pid < 0)
        quit("fork: %s", strerror(errno));
    if (g.pid == 0)
    {
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(err, 2);
        close(in[0]), close(in[1]), close(out[0]), close(out[1]), close(err);
        signal(SIGPIPE, SIG_DFL);
        if (chdir(g.cwd) < 0)
        {
            fprintf(stderr, "akern-agent: cannot enter %s: %s\n", g.cwd, strerror(errno));
            _exit(127);
        }
        execl("/bin/sh", "sh", "-c", g.agent, (char *)NULL);
        fprintf(stderr, "akern-agent: cannot run /bin/sh: %s\n", strerror(errno));
        _exit(127);
    }
    close(in[0]), close(out[1]), close(err);
    g.to_agent = in[1];
    g.from_agent = out[0];
    fcntl(g.to_agent, F_SETFD, FD_CLOEXEC);
    fcntl(g.from_agent, F_SETFD, FD_CLOEXEC);
}

/* The end of the agent's stderr, on ours, after it failed. */
static void repeat_agent_stderr(void)
{
    buf path = {0};
    buf_printf(&path, "%s.stderr", g.file);
    int fd = open(buf_cstr(&path), O_RDONLY);
    if (fd >= 0)
    {
        off_t size = lseek(fd, 0, SEEK_END);
        off_t from = size > STDERR_TAIL_BYTES ? size - STDERR_TAIL_BYTES : 0;
        char tail[STDERR_TAIL_BYTES];
        ssize_t n = pread(fd, tail, sizeof tail, from);
        if (n > 0)
        {
            const char *start = tail;
            if (from > 0 && memchr(tail, '\n', (size_t)n))
                start = (char *)memchr(tail, '\n', (size_t)n) + 1;
            say("the end of the agent's stderr (all of it in %s):", buf_cstr(&path));
            write_all(2, (span){start, (size_t)(tail + n - start)});
            if (tail[n - 1] != '\n')
                write_all(2, S("\n"));
        }
        close(fd);
    }
    buf_free(&path);
}

static void send_line(json_buf *line)
{
    log_line("out", buf_span(line));
    buf_putc(line, '\n');
    if (g.to_agent >= 0 && !write_all(g.to_agent, buf_span(line)))
    {
        close(g.to_agent);
        g.to_agent = -1;
    }
    buf_free(line);
}

/* Sends a request; params is JSON text. Returns its id. */
static long long send_request(request_kind kind, const char *method, span params)
{
    long long id = ++g.next_id;
    json_buf line = {0};
    json_printf(&line, "{'jsonrpc':'2.0','id':%D,'method':%s,'params':%R}", id, method, params);
    request r = {id, kind};
    VEC_PUSH(g.requests, g.nrequests, g.caprequests, r);
    send_line(&line);
    return id;
}

static void send_notification(const char *method, span params)
{
    json_buf line = {0};
    json_printf(&line, "{'jsonrpc':'2.0','method':%s,'params':%R}", method, params);
    send_line(&line);
}

static void respond(json id, const char *result)
{
    json_buf line = {0};
    json_printf(&line, "{'jsonrpc':'2.0','id':%R,'result':%r}", id, result);
    send_line(&line);
}

static void respond_error(json id, int code, const char *message)
{
    json_buf line = {0};
    json_printf(
        &line,
        "{'jsonrpc':'2.0','id':%R,'error':{'code':%d,'message':%s}}",
        id,
        code,
        message
    );
    send_line(&line);
}

/* ---- what the agent says ---- */

static message *message_for(json id)
{
    char *key = json_decode_dup(id);
    if (!key)
        return NULL;
    for (size_t i = 0; i < g.nmessages; i++)
        if (!strcmp(g.messages[i].id, key))
        {
            free(key);
            return &g.messages[i];
        }
    message m = {key, {0}};
    VEC_PUSH(g.messages, g.nmessages, g.capmessages, m);
    return &g.messages[g.nmessages - 1];
}

static void put_text_block(buf *text, json block)
{
    if (json_eq(json_get(block, "type"), "text"))
        json_decode(json_get(block, "text"), text);
}

static void forget_messages(void)
{
    for (size_t i = 0; i < g.nmessages; i++)
    {
        free(g.messages[i].id);
        buf_free(&g.messages[i].text);
    }
    g.nmessages = 0;
}

/* At the end of foreground work: its last agent message with text, as `akern -p` prints a run's. */
static void print_answer(void)
{
    for (size_t i = g.nmessages; i-- > 0;)
        if (g.messages[i].text.n)
        {
            buf line = {0};
            buf_putspan(&line, buf_span(&g.messages[i].text));
            if (line.p[line.n - 1] != '\n')
                buf_putc(&line, '\n');
            write_all(1, buf_span(&line));
            buf_free(&line);
            break;
        }
    forget_messages();
}

static void on_state(json update)
{
    json state = json_get(update, "state");
    g.seen_state = true;
    g.state_seq = g.seq;
    g.quiet_since = 0;
    g.idle = json_eq(state, "idle");
    if (!g.idle)
    {
        g.busy_seq = g.seq;
        return;
    }
    json background = json_get(update, "background");
    if (json_is_null_or_absent(background))
        g.background_quiet = true; /* not reported: nothing to wait for that can be known */
    else
    {
        jiter it = json_iter(background);
        json item;
        g.background_quiet = !json_next(&it, NULL, &item);
    }
    json stop = json_get(update, "stopReason");
    free(g.stop);
    g.stop = json_decode_dup(stop);
    g.failed = json_eq(stop, "error");
    if (g.failed)
    {
        buf text = {0};
        if (!json_decode(json_get(json_get(update, "error"), "message"), &text))
            buf_puts(&text, "the agent's work ended in an error");
        say("%s", buf_cstr(&text));
        buf_free(&text);
    }
    print_answer();
}

static void on_update(json params)
{
    json session = json_get(params, "sessionId");
    if (!g.session_id || !json_eq(session, g.session_id))
        return; /* a child session's: it is in the file */
    json update = json_get(params, "update");
    json kind = json_get(update, "sessionUpdate");
    if (json_eq(kind, "agent_message"))
    {
        message *m = message_for(json_get(update, "messageId"));
        if (!m)
            return;
        m->text.n = 0;
        jiter it = json_iter(json_get(update, "content"));
        for (json block; json_next(&it, NULL, &block);)
            put_text_block(&m->text, block);
    }
    else if (json_eq(kind, "agent_message_chunk"))
    {
        message *m = message_for(json_get(update, "messageId"));
        if (m)
            put_text_block(&m->text, json_get(update, "content"));
    }
    else if (json_eq(kind, "state_update"))
        on_state(update);
}

/* What a permission request is for, in a line: its title, and the command when it names one. */
static void describe_permission(json params, buf *out)
{
    if (!json_decode(json_get(params, "title"), out))
    {
        /* protocol 1's shape, from an agent that answered 2 but still asks the old way */
        json call = json_get(params, "toolCall");
        if (!json_decode(json_get(call, "title"), out))
            buf_puts(out, "an action");
    }
    json subject = json_get(params, "subject");
    if (json_eq(json_get(subject, "type"), "command"))
    {
        buf_puts(out, ": ");
        json_decode(json_get(subject, "command"), out);
    }
}

/* The first offered option of one of these kinds, or NOSPAN. */
static json pick_option(json options, const char *const kinds[2])
{
    for (int k = 0; k < 2; k++)
    {
        jiter it = json_iter(options);
        for (json o; json_next(&it, NULL, &o);)
            if (json_eq(json_get(o, "kind"), kinds[k]))
                return json_get(o, "optionId");
    }
    return NOSPAN;
}

static void cancel(void)
{
    json_buf params = {0};
    json_printf(&params, "{'sessionId':%s}", g.session_id);
    send_notification("session/cancel", buf_span(&params));
    buf_free(&params);
}

/* --allow grants it; otherwise it is refused, and the work it belongs to is cancelled, so the
   caller hears about it now rather than after the agent has worked around it. */
static void on_permission(json id, json params)
{
    static const char *const allow[2] = {"allow_once", "allow_always"};
    static const char *const reject[2] = {"reject_once", "reject_always"};
    json options = json_get(params, "options");
    json option = pick_option(options, g.allow ? allow : reject);
    json_buf result = {0};
    if (option.p)
        json_printf(&result, "{'outcome':{'outcome':'selected','optionId':%R}}", option);
    else
        json_printf(&result, "{'outcome':{'outcome':'cancelled'}}");
    respond(id, buf_cstr(&result));
    buf_free(&result);
    if (g.allow)
        return;
    buf what = {0};
    describe_permission(params, &what);
    strv_push(&g.refused, xstrdup(buf_cstr(&what)));
    buf_free(&what);
    cancel();
}

static void on_response(json line)
{
    long long id = (long long)json_get_num(json_get(line, "id"), -1);
    request r = {-1, REQ_SETUP};
    for (size_t i = 0; i < g.nrequests; i++)
        if (g.requests[i].id == id)
        {
            r = g.requests[i];
            g.requests[i] = g.requests[--g.nrequests];
            break;
        }
    if (r.id < 0)
        return;
    if (id == g.awaited)
    {
        buf_set(&g.response, line);
        g.responded = true;
        return;
    }
    if (r.kind != REQ_PROMPT)
        return;
    g.prompts_open--;
    g.answered_seq = g.seq;
    g.quiet_since = 0;
    json error = json_get(line, "error");
    if (!error.p)
    {
        g.inserted++;
        return;
    }
    buf text = {0};
    json_decode(json_get(error, "message"), &text);
    say("the agent did not take the prompt: %s", text.n ? buf_cstr(&text) : "no reason given");
    buf_free(&text);
    if (!g.inserted)
    {
        g.fatal = true;
        g.fatal_code = EXIT_FAILED;
    }
}

static void on_agent_line(span line)
{
    g.seq++;
    log_line("in", line);
    if (!json_is_valid(line))
        return; /* not ACP: an agent that writes logs to stdout */
    json method = json_get(line, "method"), id = json_get(line, "id");
    if (method.p && id.p && !json_is_null_or_absent(id))
    {
        if (json_eq(method, "session/request_permission"))
            on_permission(id, json_get(line, "params"));
        else
            respond_error(id, -32601, "akern-agent does not offer this method");
    }
    else if (method.p)
    {
        if (json_eq(method, "session/update"))
            on_update(json_get(line, "params"));
    }
    else if (id.p)
        on_response(line);
}

/* Reads what the agent wrote and handles each whole line; false at EOF. */
static bool read_agent(void)
{
    char chunk[65536];
    ssize_t n = read(g.from_agent, chunk, sizeof chunk);
    if (n < 0 && errno == EINTR)
        return true;
    if (n <= 0)
        return false;
    buf_put(&g.from_partial, chunk, (size_t)n);
    size_t start = 0;
    for (size_t i = 0; i < g.from_partial.n; i++)
        if (g.from_partial.p[i] == '\n')
        {
            on_agent_line((span){g.from_partial.p + start, i - start});
            start = i + 1;
        }
    buf_drop_front(&g.from_partial, start);
    return true;
}

/* Waits up to `seconds` for the agent to write; false at EOF. */
static bool wait_agent(double seconds)
{
    struct pollfd p = {g.from_agent, POLLIN, 0};
    int ready = poll(&p, 1, seconds < 0 ? -1 : (int)(seconds * 1000));
    if (ready <= 0)
        return true;
    return read_agent();
}

static void agent_gone(const char *when)
{
    int status = 0;
    waitpid(g.pid, &status, 0);
    g.pid = 0;
    if (WIFEXITED(status))
        say("the agent exited %s (exit %d)", when, WEXITSTATUS(status));
    else
        say("the agent exited %s (signal %d)", when, WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    repeat_agent_stderr();
}

/* A setup request, waited for: its response line, or a fatal exit. */
static json call(const char *method, span params)
{
    g.awaited = send_request(REQ_SETUP, method, params);
    g.responded = false;
    while (!g.responded)
    {
        if (signalled)
            exit(EXIT_SIGNAL);
        if (!wait_agent(-1))
        {
            agent_gone("during setup");
            exit(EXIT_USAGE);
        }
    }
    g.awaited = -1;
    json response = buf_span(&g.response);
    json error = json_get(response, "error");
    if (error.p)
    {
        buf text = {0};
        json_decode(json_get(error, "message"), &text);
        say("%s failed: %s", method, text.n ? buf_cstr(&text) : "no reason given");
        if (json_get_num(json_get(error, "code"), 0) == -32000)
            say("the agent needs you to log in: run it once on a terminal");
        exit(EXIT_USAGE);
    }
    return json_get(response, "result");
}

/* ---- setup ---- */

static void initialize(void)
{
    json result = call(
        "initialize",
        S("{\"protocolVersion\":2,\"info\":{\"name\":\"akern-agent\",\"version\":\"0\"},"
          "\"capabilities\":{}}")
    );
    double version = json_get_num(json_get(result, "protocolVersion"), 0);
    if (version != 2)
    {
        say("the agent speaks ACP protocol %g; akern-agent needs protocol 2", version);
        exit(EXIT_USAGE);
    }
}

/* The session's config options, from the setup response, kept for -o. */
static buf config_options;

static void open_session(bool resuming)
{
    json_buf params = {0};
    if (resuming)
        json_printf(&params, "{'sessionId':%s,'cwd':%s}", g.session_id, g.cwd);
    else
        json_printf(&params, "{'cwd':%s}", g.cwd);
    json result = call(resuming ? "session/resume" : "session/new", buf_span(&params));
    buf_free(&params);
    if (!resuming)
    {
        g.session_id = json_decode_dup(json_get(result, "sessionId"));
        if (!g.session_id)
        {
            say("session/new answered with no sessionId");
            exit(EXIT_USAGE);
        }
        write_header();
    }
    buf_set(&config_options, json_get(result, "configOptions"));
}

static bool is_ascii_ieq(span a, const char *b)
{
    return a.n == strlen(b) && !strncasecmp(a.p, b, a.n);
}

/* Finds the choice of a select option that `want` names: its value, else (ignoring case) its
   value or name. Groups are searched through. Lists every choice in `choices` as it goes. */
static void find_choice(json list, const char *want, bool exact, json *found, buf *choices)
{
    jiter it = json_iter(list);
    for (json o; json_next(&it, NULL, &o);)
    {
        json nested = json_get(o, "options");
        if (nested.p)
        {
            find_choice(nested, want, exact, found, choices);
            continue;
        }
        json value = json_get(o, "value");
        buf v = {0}, name = {0};
        json_decode(value, &v);
        json_decode(json_get(o, "name"), &name);
        if (choices && v.n)
        {
            buf_puts(choices, choices->n ? ", " : "");
            buf_putspan(choices, buf_span(&v));
            if (name.n && strcmp(buf_cstr(&name), buf_cstr(&v)))
                buf_printf(choices, " (%s)", buf_cstr(&name));
        }
        bool match = exact
                         ? !strcmp(buf_cstr(&v), want)
                         : is_ascii_ieq(buf_span(&v), want) || is_ascii_ieq(buf_span(&name), want);
        if (match && !found->p)
            *found = value;
        buf_free(&v);
        buf_free(&name);
    }
}

/* -o KEY=VALUE: the option whose configId, else whose category, is KEY, set to VALUE. */
static void set_option(const char *assignment)
{
    const char *eq = strchr(assignment, '=');
    span key = {assignment, (size_t)(eq - assignment)};
    const char *want = eq + 1;
    json option = NOSPAN;
    buf names = {0};
    for (int pass = 0; pass < 2 && !option.p; pass++)
    {
        jiter it = json_iter(buf_span(&config_options));
        for (json o; json_next(&it, NULL, &o);)
        {
            json id = json_get(o, pass ? "category" : "configId");
            buf text = {0};
            json_decode(id, &text);
            if (!option.p && text.n == key.n && !memcmp(text.p, key.p, key.n))
                option = o;
            if (!pass)
                buf_printf(&names, "%s%s", names.n ? ", " : "", buf_cstr(&text));
            buf_free(&text);
        }
    }
    if (!option.p)
    {
        say("-o %s: the agent has no option %.*s; it has: %s",
            assignment,
            (int)key.n,
            key.p,
            names.n ? buf_cstr(&names) : "none");
        exit(EXIT_USAGE);
    }
    buf_free(&names);
    json_buf params = {0};
    json config_id = json_get(option, "configId");
    if (json_eq(json_get(option, "type"), "boolean"))
    {
        bool on = !strcmp(want, "true") || !strcmp(want, "on") || !strcmp(want, "1");
        if (!on && strcmp(want, "false") && strcmp(want, "off") && strcmp(want, "0"))
        {
            say("-o %s: %.*s is true or false", assignment, (int)key.n, key.p);
            exit(EXIT_USAGE);
        }
        json_printf(
            &params,
            "{'sessionId':%s,'configId':%R,'type':'boolean','value':%b}",
            g.session_id,
            config_id,
            on
        );
    }
    else
    {
        json value = NOSPAN;
        buf choices = {0};
        find_choice(json_get(option, "options"), want, true, &value, &choices);
        if (!value.p)
            find_choice(json_get(option, "options"), want, false, &value, NULL);
        if (!value.p)
        {
            say("-o %s: no such choice; the choices are: %s", assignment, buf_cstr(&choices));
            exit(EXIT_USAGE);
        }
        buf_free(&choices);
        json_printf(
            &params,
            "{'sessionId':%s,'configId':%R,'type':'id','value':%R}",
            g.session_id,
            config_id,
            value
        );
    }
    json result = call("session/set_config_option", buf_span(&params));
    buf_set(&config_options, json_get(result, "configOptions"));
    buf_free(&params);
}

/* ---- prompts and stdin ---- */

static void send_prompt(span text)
{
    json_buf params = {0};
    json_printf(
        &params,
        "{'sessionId':%s,'prompt':[{'type':'text','text':%S}]}",
        g.session_id,
        text
    );
    send_request(REQ_PROMPT, "session/prompt", buf_span(&params));
    buf_free(&params);
    g.prompts_open++;
    g.sent_seq = g.seq;
    g.quiet_since = 0;
}

/* A line on stdin, as `akern -p` takes them: plain text is a message for the agent; a send line
   with when "now" interrupts it first; an abort line cancels its work. */
static void on_stdin_line(span line)
{
    if (line.n && line.p[line.n - 1] == '\r')
        line.n--;
    if (!line.n)
        return;
    if (line.p[0] == '{' && json_is_valid(line))
    {
        json type = json_get(line, "type");
        if (json_eq(type, "abort"))
        {
            cancel();
            return;
        }
        if (json_eq(type, "send"))
        {
            buf text = {0};
            if (!json_decode(json_get(line, "content"), &text))
            {
                say("a send line needs a content string");
                return;
            }
            if (json_eq(json_get(line, "when"), "now"))
                cancel();
            send_prompt(buf_span(&text));
            buf_free(&text);
            return;
        }
    }
    send_prompt(line);
}

static void read_stdin(void)
{
    char chunk[65536];
    ssize_t n = read(0, chunk, sizeof chunk);
    if (n < 0 && errno == EINTR)
        return;
    if (n <= 0)
    {
        g.stdin_open = false;
        if (g.in_partial.n)
            on_stdin_line(buf_span(&g.in_partial));
        buf_free(&g.in_partial);
        return;
    }
    buf_put(&g.in_partial, chunk, (size_t)n);
    size_t start = 0;
    for (size_t i = 0; i < g.in_partial.n; i++)
        if (g.in_partial.p[i] == '\n')
        {
            on_stdin_line((span){g.in_partial.p + start, i - start});
            start = i + 1;
        }
    buf_drop_front(&g.in_partial, start);
}

/* Done: every prompt taken, and the agent idle with nothing in the background, in a state it
   reported after the last prompt was taken, or after work it started once it was sent. */
static bool is_quiet(void)
{
    return g.prompts_open == 0 && g.inserted > 0 && g.seen_state && g.idle && g.background_quiet &&
           (g.state_seq > g.answered_seq || g.busy_seq > g.sent_seq);
}

/* ---- the end ---- */

static void reap(double seconds)
{
    for (double until = now() + seconds; g.pid > 0 && now() < until;)
    {
        if (waitpid(g.pid, NULL, WNOHANG) == g.pid)
            g.pid = 0;
        else if (g.from_agent >= 0)
        {
            if (!wait_agent(0.05))
            {
                close(g.from_agent);
                g.from_agent = -1;
            }
        }
        else
        {
            struct timespec t = {0, 50 * 1000 * 1000};
            nanosleep(&t, NULL);
        }
    }
}

/* Closes the agent's stdin, which ends the connection, and waits for it to exit. */
static void stop_agent(void)
{
    if (g.to_agent >= 0)
        close(g.to_agent), g.to_agent = -1;
    reap(CLOSE_SECONDS);
    if (g.pid > 0)
    {
        kill(g.pid, SIGTERM);
        reap(TERM_SECONDS);
    }
    if (g.pid > 0)
    {
        kill(g.pid, SIGKILL);
        waitpid(g.pid, NULL, 0);
        g.pid = 0;
    }
}

static int exit_code(void)
{
    if (g.fatal)
        return g.fatal_code;
    if (g.refused.n)
    {
        for (size_t i = 0; i < g.refused.n; i++)
            say("refused permission for %s", g.refused.v[i]);
        say("to grant it, continue with --allow: akern-agent --allow -s %s 'Go ahead.'", g.file);
        return EXIT_REFUSED;
    }
    if (g.failed)
        return EXIT_FAILED;
    if (g.stop && strcmp(g.stop, "end_turn") && strcmp(g.stop, "cancelled"))
        say("the agent stopped: %s", g.stop);
    return EXIT_OK;
}

static void run(void)
{
    for (;;)
    {
        if (signalled)
            return;
        bool quiet = is_quiet();
        if (!quiet)
            g.quiet_since = 0;
        else if (!g.quiet_since)
            g.quiet_since = now();
        if (g.fatal || (quiet && now() - g.quiet_since >= QUIET_SECONDS))
            return;
        struct pollfd p[2] = {{g.from_agent, POLLIN, 0}, {0, POLLIN, 0}};
        nfds_t n = g.stdin_open ? 2 : 1;
        int timeout = quiet ? (int)((QUIET_SECONDS - (now() - g.quiet_since)) * 1000) + 1 : -1;
        if (poll(p, n, timeout) < 0)
            continue; /* a signal */
        if (p[0].revents && !read_agent())
        {
            agent_gone("before it was done");
            g.fatal = true;
            g.fatal_code = EXIT_FAILED;
            return;
        }
        if (n > 1 && p[1].revents)
            read_stdin();
    }
}

static void usage(int code)
{
    fputs(
        "usage: akern-agent [-a AGENT_COMMAND] [-s SESSION.jsonl] [-C DIR] [-m MODEL] [-o "
        "ID=VALUE]...\n"
        "                   [--allow] PROMPT...\n",
        code ? stderr : stdout
    );
    exit(code);
}

int main(int argc, char **argv)
{
    static const struct option long_options[] = {
        {"agent", required_argument, NULL, 'a'},
        {"session", required_argument, NULL, 's'},
        {"cwd", required_argument, NULL, 'C'},
        {"model", required_argument, NULL, 'm'},
        {"option", required_argument, NULL, 'o'},
        {"allow", no_argument, NULL, 'y'},
        {"help", no_argument, NULL, 'h'},
        {0},
    };
    for (int ch; (ch = getopt_long(argc, argv, "+a:s:C:m:o:yh", long_options, NULL)) != -1;)
    {
        buf assignment = {0};
        switch (ch)
        {
        case 'a':
            g.agent = optarg;
            break;
        case 's':
            g.file = optarg;
            break;
        case 'C':
            g.cwd = optarg;
            break;
        case 'm':
            buf_printf(&assignment, "model=%s", optarg);
            strv_push(&g.options, xstrdup(buf_cstr(&assignment)));
            buf_free(&assignment);
            break;
        case 'o':
            if (!strchr(optarg, '=') || optarg[0] == '=')
                usage(EXIT_USAGE);
            strv_push(&g.options, xstrdup(optarg));
            break;
        case 'y':
            g.allow = true;
            break;
        case 'h':
            usage(EXIT_OK);
            break;
        default:
            usage(EXIT_USAGE);
        }
    }
    buf prompt = {0};
    for (int i = optind; i < argc; i++)
        buf_printf(&prompt, "%s%s", i > optind ? " " : "", argv[i]);
    if (!prompt.n)
        usage(EXIT_USAGE);

    bool resuming = open_session_file();
    if (!g.agent)
        quit("no agent: pass -a COMMAND, for example -a akern-acp");
    char resolved[PATH_MAX];
    if (!g.cwd)
        g.cwd = getcwd(resolved, sizeof resolved);
    else if (realpath(g.cwd, resolved))
        g.cwd = resolved;
    else
        quit("cannot use %s as the working directory: %s", g.cwd, strerror(errno));
    if (!g.cwd)
        quit("getcwd: %s", strerror(errno));
    g.cwd = xstrdup(g.cwd);

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    g.stdin_open = true;
    start_agent();
    initialize();
    open_session(resuming);
    for (size_t i = 0; i < g.options.n; i++)
        set_option(g.options.v[i]);
    send_prompt(buf_span(&prompt));
    run();
    if (signalled)
    {
        if (g.session_id)
            cancel();
        stop_agent();
        return EXIT_SIGNAL;
    }
    stop_agent();
    return exit_code();
}
