/* akern-acp: an ACP agent (agentclientprotocol.com) in front of akern, for editors and hosts
   that speak ACP (Zed, Omnigent's acp:<slug> agents, ...). JSON-RPC 2.0 lines on stdin and
   stdout; one akern per ACP session, its file <sessions>/<id>.jsonl, so a session resumes after
   a restart. Owns the handshake, sessions (new, load, model), turning a prompt into a send and
   akern's events into session/update notifications, cancel as abort, and when a prompt is
   answered. Knows nothing of models or tools beyond the names shell, edit and job (tools.md).

   Protocol 1: a prompt is answered when the run it started ends (--until run), or once akern
   has settled: no run, nothing queued, no job running (--until settled). What akern does while
   no prompt is open (a job ends and wakes the model) is held and sent when the next prompt
   starts (--background hold), or sent at once (--background send), for clients that show
   updates between prompts.

   Protocol 2 (a draft): a prompt is answered once akern delivers it into the conversation, and
   may come while akern works, so it steers the run at its next turn. Message ids are akern's:
   a prompt's is its inbox item's, an assistant message's its entry's, so they match on replay.
   A run is the foreground: state_update running, then idle with its stop reason; jobs that outlive
   it are the background, listed on each state_update, and say what they do whenever they do it. A
   shell call is a terminal its output streams to. A shell call that runs a sub-agent (akern -s
   FILE, akern.md §5) is a child session, its file read as it grows. */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "json.h"

enum { TAIL_BYTES = 16384, TITLE_BYTES = 120, MAX_SESSIONS = 64, MAX_PROVIDERS = 16 };
/* A terminal's output not yet sent is sent at once past this. */
enum { OUTPUT_FLUSH_BYTES = 1 << 20 };
/* How often a running sub-agent's session file is read for what it added. */
static const double CHILD_POLL_SECONDS = 0.2;
/* The prefix of akern-acp's own _meta keys (protocol 2 wants a reverse-DNS one). */
#define META "io.github.wrongbad.akern/"
/* How long a provider may take to list its models (as akern-model allows, tools.md §4). */
static const double MODELS_TIMEOUT_SECONDS = 10;
/* The effort levels offered as a choice; /effort also takes any other value the provider does. */
static const char *const EFFORT_LEVELS[] = {"low", "medium", "high"};
static const double PROGRESS_EVERY_SECONDS = 0.25;
/* Clients time out a prompt with no updates (Omnigent's ACP client after 300 s): while a call
   runs silently, its card is touched this often. */
static const double KEEPALIVE_SECONDS = 60;

struct child;

typedef struct {
    char *id, *name;
    buf tail;       /* protocol 1: the end of its output so far; 2: a terminal's output not yet sent */
    json_buf diff;  /* protocol 1: an edit's diff content item, or empty */
    json_buf input; /* protocol 2: an edit's input, for its patch */
    bool dirty;
    bool terminal;  /* protocol 2: shown as a terminal, a shell call that started */
    bool cancelled; /* protocol 2: its process group was signalled by tool_call/cancel */
    double sent_at;
    long long run;
    pid_t pid;      /* its process group; 0 when none is known */
    struct child *child;
} call;

/* Protocol 2: a sub-agent, a session of its own whose file is read as it grows. */
typedef struct child {
    char *id, *parent, *path, *title; /* parent: the session it was announced on */
    off_t offset;
    buf partial;
    bool running;
    call *calls;
    size_t ncalls, capcalls;
} child;

/* Protocol 2: a prompt sent to akern, answered when akern delivers its item into the
   conversation, or refused when an abort hands it back first. */
typedef struct {
    long long ref;
    char *item; /* its inbox item's id, from send's ok; NULL until then */
    json_buf request, prompt;
} pending;

typedef enum { PROMPT_NONE, PROMPT_SENT, PROMPT_QUEUED } prompt_state;

typedef struct {
    char *id, *cwd, *path, *model;
    char *effort; /* the provider's effort param in this session; NULL for its default */
    pid_t pid;
    int to_child, from_child, child_err;
    buf out_partial, err_partial, err_text;
    bool ready, exited, run_open;
    bool settled, restart; /* akern is idle; it is to start again with the model and effort */
    json_buf open_request; /* id of a session/new or session/load awaiting hello */
    prompt_state prompt;
    json_buf prompt_id;
    long long ref;
    bool cancelled;
    char *prompt_item;     /* protocol 1: the open prompt's inbox item, from send's ok */
    bool prompt_delivered; /* protocol 1: akern has put that item into the conversation */
    long long usage[4]; /* input, output, cache_read, cache_write */
    double updated_at;  /* when the last update went out during a prompt */
    buf held;           /* notification lines held while no prompt is open */
    bool streamed;      /* this turn's text arrived as deltas */
    call *calls;
    size_t ncalls, capcalls;
    /* protocol 2 */
    char *title;         /* reported in session_info_update; NULL until known */
    long long run;       /* the latest run */
    json_buf message_id; /* the assistant message of the open turn: turn_start's id */
    bool resuming, closing;
    json_buf close_request;
    pending *pending;
    size_t npending, cappending;
    child **children;
    size_t nchildren, capchildren;
} session;

/* What `<provider> --models` listed, asked once per provider. */
typedef struct {
    char *provider, *error;
    strv ids;
} model_list;

static struct {
    session *sessions[MAX_SESSIONS];
    size_t nsessions;
    model_list lists[MAX_PROVIDERS];
    size_t nlists;
    buf in_partial;
    bool until_settled, send_background, stdin_closed;
    bool v2;          /* the client asked for protocol 2 */
    double polled_at; /* when sub-agents' files were last read */
    char *sessions_dir;
    strv akern_args;
} g;

static double now_seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("akern-acp: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---- JSON-RPC out ---- */

static void write_line(json_buf *line)
{
    buf_putc(line, '\n');
    if (!write_all(STDOUT_FILENO, buf_span(line)))
        exit(0);
    buf_free(line);
}

static void respond(json id, const char *fmt, ...)
{
    json_buf result = {0}, line = {0};
    va_list ap;
    va_start(ap, fmt);
    json_vprintf(&result, fmt, ap);
    va_end(ap);
    json_printf(&line, "{'jsonrpc':'2.0','id':%R,'result':%R}", id, buf_span(&result));
    write_line(&line);
    buf_free(&result);
}

static void respond_error(json id, int code, span message)
{
    json_buf line = {0};
    json_printf(
        &line,
        "{'jsonrpc':'2.0','id':%R,'error':{'code':%d,'message':%S}}",
        id,
        code,
        message
    );
    write_line(&line);
}

/* A session/update, sent at once while a prompt is open (or when asked to), else held. */
static void update(session *s, bool now, const char *fmt, ...)
{
    json_buf body = {0}, line = {0};
    va_list ap;
    va_start(ap, fmt);
    json_vprintf(&body, fmt, ap);
    va_end(ap);
    json_printf(
        &line,
        "{'jsonrpc':'2.0','method':'session/update','params':{'sessionId':%s,'update':%R}}",
        s->id,
        buf_span(&body)
    );
    if (g.v2 || now || s->prompt != PROMPT_NONE || g.send_background)
        write_line(&line), s->updated_at = now_seconds();
    else
    {
        buf_putspan(&s->held, buf_span(&line));
        buf_putc(&s->held, '\n');
        buf_free(&line);
    }
    buf_free(&body);
}

/* Protocol 2: a session/update for any session, a child included, sent at once. */
static void notify(const char *session_id, const char *fmt, ...)
{
    json_buf body = {0}, line = {0};
    va_list ap;
    va_start(ap, fmt);
    json_vprintf(&body, fmt, ap);
    va_end(ap);
    json_printf(
        &line,
        "{'jsonrpc':'2.0','method':'session/update','params':{'sessionId':%s,'update':%R}}",
        session_id,
        buf_span(&body)
    );
    write_line(&line);
    buf_free(&body);
}

static void flush_held(session *s)
{
    if (s->held.n && !write_all(STDOUT_FILENO, buf_span(&s->held)))
        exit(0);
    s->held.n = 0;
}

/* ---- the akern child ---- */

static void send_to_child(session *s, const char *fmt, ...)
{
    json_buf line = {0};
    va_list ap;
    va_start(ap, fmt);
    json_vprintf(&line, fmt, ap);
    va_end(ap);
    buf_putc(&line, '\n');
    if (s->to_child < 0 || !write_all(s->to_child, buf_span(&line)))
        warn("session %s: akern is not reading", s->id);
    buf_free(&line);
}

static void cloexec_pipe(int fds[2])
{
    if (pipe(fds) < 0)
        die("pipe: %s", strerror(errno));
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
}

static char *new_id(void);

static bool spawn_akern(session *s)
{
    int in[2], out[2], err[2];
    cloexec_pipe(in);
    cloexec_pipe(out);
    cloexec_pipe(err);
    strv argv = {0};
    strv_push(&argv, xstrdup("akern"));
    strv_push(&argv, xstrdup("-s"));
    strv_push(&argv, xstrdup(s->path));
    for (size_t i = 0; i < g.akern_args.n; i++)
        strv_push(&argv, xstrdup(g.akern_args.v[i]));
    if (s->model) /* the session's model and effort: akern has them only from its flags */
        strv_push(&argv, xstrdup("-m")), strv_push(&argv, xstrdup(s->model));
    if (s->effort)
    {
        buf param = {0};
        buf_printf(&param, "effort=%s", s->effort);
        strv_push(&argv, xstrdup("-P")), strv_push(&argv, buf_cstr(&param));
    }
    strv_push(&argv, NULL);
    pid_t pid = fork();
    if (pid == 0)
    {
        dup2(in[0], STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        dup2(err[1], STDERR_FILENO);
        signal(SIGPIPE, SIG_DFL);
        if (s->cwd && chdir(s->cwd) < 0)
        {
            fprintf(stderr, "akern-acp: %s: %s\n", s->cwd, strerror(errno));
            _exit(2);
        }
        execvp("akern", argv.v);
        fprintf(stderr, "akern-acp: akern: %s\n", strerror(errno));
        _exit(127);
    }
    strv_free(&argv);
    close(in[0]);
    close(out[1]);
    close(err[1]);
    if (pid < 0)
    {
        close(in[1]), close(out[0]), close(err[0]);
        return false;
    }
    s->pid = pid;
    s->to_child = in[1];
    s->from_child = out[0];
    s->child_err = err[0];
    s->ready = s->exited = s->run_open = s->restart = false;
    s->settled = true;
    s->out_partial.n = s->err_partial.n = s->err_text.n = 0;
    return true;
}

/* ---- sessions ---- */

static session *find_session(json id)
{
    for (size_t i = 0; i < g.nsessions; i++)
        if (json_eq(id, g.sessions[i]->id))
            return g.sessions[i];
    return NULL;
}

static bool is_safe_id(const char *id)
{
    if (!*id || strlen(id) > 128)
        return false;
    for (const char *p = id; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_')
            return false;
    return true;
}

static char *new_id(void)
{
    unsigned char bytes[8];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0 || read(fd, bytes, sizeof bytes) != (ssize_t)sizeof bytes)
        for (size_t i = 0; i < sizeof bytes; i++)
            bytes[i] = (unsigned char)(rand() ^ (int)getpid() ^ (int)time(NULL));
    if (fd >= 0)
        close(fd);
    buf id = {0};
    for (size_t i = 0; i < sizeof bytes; i++)
        buf_printf(&id, "%02x", bytes[i]);
    return buf_cstr(&id);
}

static void make_dirs(const char *path)
{
    buf dir = {0};
    for (const char *p = strchr(path + 1, '/'); p; p = strchr(p + 1, '/'))
    {
        dir.n = 0;
        buf_put(&dir, path, (size_t)(p - path));
        mkdir(buf_cstr(&dir), 0755);
    }
    mkdir(path, 0755);
    buf_free(&dir);
}

static session *add_session(const char *id, json cwd)
{
    if (g.nsessions == MAX_SESSIONS)
        return NULL;
    session *s = xcalloc(1, sizeof *s);
    s->id = xstrdup(id);
    s->cwd = json_decode_dup(cwd);
    buf path = {0};
    buf_printf(&path, "%s/%s.jsonl", g.sessions_dir, id);
    s->path = buf_cstr(&path);
    s->to_child = s->from_child = s->child_err = -1;
    g.sessions[g.nsessions++] = s;
    return s;
}

static call *find_in(call *calls, size_t n, json id)
{
    for (size_t i = 0; i < n; i++)
        if (json_eq(id, calls[i].id))
            return &calls[i];
    return NULL;
}

static call *find_call(session *s, json id)
{
    return find_in(s->calls, s->ncalls, id);
}

static call *push_call(call **calls, size_t *n, size_t *cap, json id, json name)
{
    call c = {.id = json_decode_dup(id), .name = json_decode_dup(name)};
    if (!c.id)
        c.id = xstrdup("");
    VEC_PUSH(*calls, *n, *cap, c);
    return &(*calls)[*n - 1];
}

static void drop_call(call *calls, size_t *n, call *c)
{
    free(c->id);
    free(c->name);
    buf_free(&c->tail);
    buf_free(&c->diff);
    buf_free(&c->input);
    *c = calls[--*n];
}

static void forget_call(session *s, call *c)
{
    drop_call(s->calls, &s->ncalls, c);
}

/* ---- tool calls as ACP tool calls ---- */

static const char *kind_of(json name)
{
    return json_eq(name, "shell") ? "execute" : json_eq(name, "edit") ? "edit" : "other";
}

/* An absolute path for a path the model gave relative to the session's directory. */
static void put_absolute(buf *out, session *s, json path)
{
    buf text = {0};
    json_decode(path, &text);
    if (text.n && text.p[0] != '/' && s->cwd)
        buf_printf(out, "%s/", s->cwd);
    buf_putspan(out, buf_span(&text));
    buf_free(&text);
}

/* The first line of a text, cut to TITLE_BYTES on a character boundary. */
static void put_short(buf *out, buf *text)
{
    size_t n = strcspn(buf_cstr(text), "\n");
    if (n > TITLE_BYTES)
        n = TITLE_BYTES;
    while (n < text->n && ((unsigned char)text->p[n] & 0xC0) == 0x80)
        n--;
    buf_put(out, text->p, n);
    if (n < text->n)
        buf_puts(out, "…");
}

static void put_title(buf *title, json name, json input)
{
    buf text = {0};
    if (json_eq(name, "shell"))
        json_decode(json_get(input, "command"), &text);
    else if (json_eq(name, "edit"))
        buf_puts(&text, "edit "), json_decode(json_get(input, "path"), &text);
    else
    {
        json_decode(name, &text);
        if (json_is(json_get(input, "id"), '"'))
            buf_putc(&text, ' '), json_decode(json_get(input, "id"), &text);
    }
    put_short(title, &text);
    buf_cstr(title); /* an empty title is "", not null */
    buf_free(&text);
}

/* tool_call for a tool_use: its title, kind, input, and for an edit its file and diff. */
static void open_call(session *s, json id, json name, json input, bool now)
{
    buf title = {0}, path = {0};
    json_buf extra = {0};
    put_title(&title, name, input);
    call c = {.id = json_decode_dup(id), .name = json_decode_dup(name)};
    if (!c.id)
        c.id = xstrdup("");
    if (json_eq(name, "edit") && json_is(json_get(input, "path"), '"'))
    {
        put_absolute(&path, s, json_get(input, "path"));
        buf old_text = {0}, new_text = {0};
        json_decode(json_get(input, "old"), &old_text);
        json_decode(json_get(input, "new"), &new_text);
        json_printf(
            &c.diff,
            "{'type':'diff','path':%S,'oldText':%S,'newText':%S}",
            buf_span(&path),
            buf_span(&old_text),
            buf_span(&new_text)
        );
        json_printf(
            &extra,
            ",'locations':[{'path':%S}],'content':[%R]",
            buf_span(&path),
            buf_span(&c.diff)
        );
        buf_free(&old_text);
        buf_free(&new_text);
    }
    VEC_PUSH(s->calls, s->ncalls, s->capcalls, c);
    update(
        s,
        now,
        "{'sessionUpdate':'tool_call','toolCallId':%R,'title':%S,'kind':%s,'status':'in_progress',"
        "'rawInput':%R%r}",
        id,
        buf_span(&title),
        kind_of(name),
        json_is(input, '{') ? input : S("{}"),
        buf_cstr(&extra)
    );
    buf_free(&title);
    buf_free(&path);
    buf_free(&extra);
}

static void put_fenced(json_buf *items, span text)
{
    buf fenced = {0};
    buf_puts(&fenced, "```\n");
    buf_putspan(&fenced, text);
    if (!text.n || text.p[text.n - 1] != '\n')
        buf_putc(&fenced, '\n');
    buf_puts(&fenced, "```");
    json_put_comma_unless_first(items);
    json_printf(items, "{'type':'content','content':{'type':'text','text':%S}}", buf_span(&fenced));
    buf_free(&fenced);
}

typedef enum { TEXT_PLAIN, TEXT_FENCED, TEXT_NONE } text_as;

/* A tool_result's content as ACP tool call content: a shell's text fenced (or left out when a
   terminal showed it), images as images. */
static void put_result_content(json_buf *items, json content, text_as as)
{
    bool fenced = as == TEXT_FENCED;
    jiter it = json_iter(content);
    for (json block; json_next(&it, NULL, &block);)
    {
        json type = json_get(block, "type"), source = json_get(block, "source");
        if (json_eq(type, "text") && as == TEXT_NONE)
            continue;
        if (json_eq(type, "text"))
        {
            buf text = {0};
            json_decode(json_get(block, "text"), &text);
            if (fenced)
                put_fenced(items, buf_span(&text));
            else
            {
                json_put_comma_unless_first(items);
                json_printf(
                    items,
                    "{'type':'content','content':{'type':'text','text':%S}}",
                    buf_span(&text)
                );
            }
            buf_free(&text);
        }
        else if (json_eq(type, "image") && json_eq(json_get(source, "type"), "base64"))
        {
            json_put_comma_unless_first(items);
            json_printf(
                items,
                "{'type':'content','content':{'type':'image','data':%R,'mimeType':%R}}",
                json_get(source, "data"),
                json_get(source, "media_type")
            );
        }
    }
}

/* tool_call_update closing the call a tool_result answers. */
static void close_call(session *s, json result, bool now)
{
    json id = json_get(result, "tool_use_id");
    call *c = find_call(s, id);
    json_buf items = {0};
    buf_putc(&items, '[');
    if (c && c->diff.n)
        json_put_comma_unless_first(&items), buf_putspan(&items, buf_span(&c->diff));
    put_result_content(
        &items,
        json_get(result, "content"),
        !c || !c->name || !strcmp(c->name, "shell") ? TEXT_FENCED : TEXT_PLAIN
    );
    buf_putc(&items, ']');
    update(
        s,
        now,
        "{'sessionUpdate':'tool_call_update','toolCallId':%R,'status':%s,'content':%R,'rawOutput':%R}",
        id,
        json_is_true(json_get(result, "is_error")) ? "failed" : "completed",
        buf_span(&items),
        result
    );
    if (c)
        forget_call(s, c);
    buf_free(&items);
}

/* A snapshot of a call's output so far. Each one supersedes the last, so none is held while no
   prompt is open: a chatty job between prompts would pile them up. */
static void send_progress(session *s, call *c)
{
    c->dirty = false;
    if (s->prompt == PROMPT_NONE && !g.send_background)
        return;
    json_buf items = {0};
    buf_putc(&items, '[');
    put_fenced(&items, buf_span(&c->tail));
    buf_putc(&items, ']');
    json_buf id = {0};
    json_put_string(&id, S(c->id));
    update(
        s,
        false,
        "{'sessionUpdate':'tool_call_update','toolCallId':%R,'status':'in_progress','content':%R}",
        buf_span(&id),
        buf_span(&items)
    );
    c->dirty = false;
    c->sent_at = now_seconds();
    buf_free(&items);
    buf_free(&id);
}

static void note_progress(session *s, json event)
{
    call *c = find_call(s, json_get(event, "call"));
    if (!c)
        return;
    json_decode(json_get(event, "data"), &c->tail);
    if (c->tail.n > TAIL_BYTES)
    {
        size_t cut = c->tail.n - TAIL_BYTES;
        while (cut < c->tail.n && ((unsigned char)c->tail.p[cut] & 0xC0) == 0x80)
            cut++;
        buf_drop_front(&c->tail, cut);
    }
    c->dirty = true;
}

/* ---- protocol 2: messages, tool calls, terminals, sub-agents ---- */

static void split_lines(buf *partial, span chunk, void (*fn)(void *, span), void *ctx);
static void close_fd(int *fd);

/* akern content (a string, or blocks) as an ACP content array: its text and images, or with
   `thinking` its thinking as text. Returns how many blocks it holds. */
static size_t put_acp_blocks(json_buf *out, json content, bool thinking)
{
    size_t n = 0;
    buf_putc(out, '[');
    if (json_is(content, '"') && !thinking)
        json_printf(out, "{'type':'text','text':%R}", content), n++;
    jiter it = json_is(content, '[') ? json_iter(content) : json_iter(S("[]"));
    for (json block; json_next(&it, NULL, &block);)
    {
        json type = json_get(block, "type"), source = json_get(block, "source");
        if (thinking && json_eq(type, "thinking"))
            json_printf(out, "%r{'type':'text','text':%R}", n++ ? "," : "", json_get(block, "thinking"));
        else if (!thinking && json_eq(type, "text"))
            json_printf(out, "%r{'type':'text','text':%R}", n++ ? "," : "", json_get(block, "text"));
        else if (!thinking && json_eq(type, "image") && json_eq(json_get(source, "type"), "base64"))
            json_printf(
                out,
                "%r{'type':'image','data':%R,'mimeType':%R}",
                n++ ? "," : "",
                json_get(source, "data"),
                json_get(source, "media_type")
            );
    }
    buf_putc(out, ']');
    return n;
}

/* An assistant message as agent_thought and agent_message upserts, each sent when it has any. */
static void send_assistant(const char *session_id, span message_id, json content)
{
    json_buf blocks = {0};
    if (put_acp_blocks(&blocks, content, true))
        notify(session_id, "{'sessionUpdate':'agent_thought','messageId':%S,'content':%R}", message_id, buf_span(&blocks));
    blocks.n = 0;
    if (put_acp_blocks(&blocks, content, false))
        notify(session_id, "{'sessionUpdate':'agent_message','messageId':%S,'content':%R}", message_id, buf_span(&blocks));
    buf_free(&blocks);
}

/* The process group of a call is signalled; its result, when it comes, closes it. */
static bool cancel_call(call *c)
{
    if (!c->pid)
        return false;
    if (kill(-c->pid, SIGTERM) < 0 && errno == ESRCH)
        kill(c->pid, SIGTERM);
    c->cancelled = true;
    return true;
}

/* The words of a shell command, quotes removed; `;`, `&`, `|` and newlines each a word "". */
static void shell_words(const char *s, strv *words)
{
    buf word = {0};
    bool in_word = false;
    for (; *s; s++)
    {
        if (*s == '\'' || *s == '"')
        {
            char quote = *s++;
            for (; *s && *s != quote; s++)
            {
                if (quote == '"' && *s == '\\' && s[1])
                    s++;
                buf_putc(&word, *s);
            }
            in_word = true;
            if (!*s)
                break;
        }
        else if (*s == '\\' && s[1])
            buf_putc(&word, *++s), in_word = true;
        else if (strchr(" \t;&|\n", *s))
        {
            if (in_word)
                strv_push(words, xstrdup(buf_cstr(&word)));
            if (*s != ' ' && *s != '\t')
                strv_push(words, xstrdup(""));
            word.n = 0, in_word = false;
        }
        else
            buf_putc(&word, *s), in_word = true;
    }
    if (in_word)
        strv_push(words, xstrdup(buf_cstr(&word)));
    buf_free(&word);
}

/* The session file of the sub-agent a shell command starts: `akern … -s FILE` (akern.md §5). */
static char *subagent_file(json command)
{
    buf text = {0};
    json_decode(command, &text);
    strv words = {0};
    shell_words(buf_cstr(&text), &words);
    char *file = NULL;
    bool in_akern = false;
    for (size_t i = 0; i < words.n && !file; i++)
    {
        const char *w = words.v[i], *base = strrchr(w, '/');
        const char *next = i + 1 < words.n && *words.v[i + 1] ? words.v[i + 1] : NULL;
        if (!*w)
            in_akern = false;
        else if (!in_akern)
            in_akern = !strcmp(base ? base + 1 : w, "akern");
        else if (!strcmp(w, "--session") && next)
            file = xstrdup(next);
        else if (!strncmp(w, "--session=", 10))
            file = xstrdup(w + 10);
        else if (w[0] == '-' && w[1] != '-')
        {
            /* a cluster of short flags: -s takes what follows it, as -m, -S and -P do */
            for (const char *f = w + 1; *f; f++)
                if (strchr("smSP", *f))
                {
                    if (*f == 's')
                        file = xstrdup(f[1] ? f + 1 : next ? next : "");
                    else if (!f[1])
                        i++;
                    break;
                }
        }
        else if (w[0] != '-')
            in_akern = false; /* akern's flags end at its first other word */
    }
    if (file && !*file)
        free(file), file = NULL;
    strv_free(&words);
    buf_free(&text);
    return file;
}

/* A sub-agent's child session, announced on its parent's: its id is the session's and its file's
   path, so a sub-agent continued by a later call is the same child. */
static child *start_child(session *s, const char *parent, const char *file)
{
    buf path = {0};
    if (file[0] != '/' && s->cwd)
        buf_printf(&path, "%s/", s->cwd);
    buf_puts(&path, file);
    child *k = NULL;
    for (size_t i = 0; i < s->nchildren && !k; i++)
        if (!strcmp(s->children[i]->path, buf_cstr(&path)))
            k = s->children[i];
    if (!k)
    {
        k = xcalloc(1, sizeof *k);
        k->path = xstrdup(buf_cstr(&path));
        size_t cwd = s->cwd ? strlen(s->cwd) : 0;
        const char *rel = cwd && !strncmp(k->path, s->cwd, cwd) && k->path[cwd] == '/' ? k->path + cwd + 1 : file;
        size_t n = strlen(rel);
        if (n > 6 && !strcmp(rel + n - 6, ".jsonl"))
            n -= 6;
        buf id = {0};
        buf_printf(&id, "%s/%.*s", s->id, (int)n, rel);
        k->id = buf_cstr(&id);
        k->title = span_dup((span){rel, n});
        k->parent = xstrdup(parent);
        VEC_PUSH(s->children, s->nchildren, s->capchildren, k);
    }
    k->running = true;
    notify(
        k->parent,
        "{'sessionUpdate':'subagent_update','sessionId':%s,'title':%s,'cwd':%s,'capabilities':%r,"
        "'state':{'state':'running'}}",
        k->id,
        k->title,
        s->cwd,
        strcmp(parent, s->id) ? "{}" : "{\"cancel\":{}}"
    );
    notify(k->id, "{'sessionUpdate':'state_update','state':'running'}");
    buf_free(&path);
    return k;
}

static const char *signal_name(int n)
{
    static const struct {
        int n;
        const char *name;
    } names[] = {{SIGHUP, "SIGHUP"},   {SIGINT, "SIGINT"},   {SIGQUIT, "SIGQUIT"}, {SIGABRT, "SIGABRT"},
                 {SIGKILL, "SIGKILL"}, {SIGPIPE, "SIGPIPE"}, {SIGALRM, "SIGALRM"}, {SIGTERM, "SIGTERM"},
                 {SIGUSR1, "SIGUSR1"}, {SIGUSR2, "SIGUSR2"}};
    for (size_t i = 0; i < LEN(names); i++)
        if (names[i].n == n)
            return names[i].name;
    return NULL;
}

/* A shell result's header ("[exit 0]", "[killed, signal 15]", tools.md §1) as an exitStatus. */
static void put_exit_status(json_buf *out, json content)
{
    buf text = {0};
    jiter it = json_iter(content);
    for (json block; json_next(&it, NULL, &block) && !text.n;)
        if (json_eq(json_get(block, "type"), "text"))
            json_decode(json_get(block, "text"), &text);
    char *line = buf_cstr(&text), *signal, *exit;
    line[strcspn(line, "\n")] = 0;
    buf_putc(out, '{');
    if (line[0] == '[' && (signal = strstr(line, "signal ")))
    {
        int n = atoi(signal + 7);
        if (signal_name(n))
            json_printf(out, "'signal':%s", signal_name(n));
        else
            json_printf(out, "'signal':'%d'", n);
    }
    else if (line[0] == '[' && (exit = strstr(line, "exit ")))
        json_printf(out, "'exitCode':%d", atoi(exit + 5));
    buf_putc(out, '}');
    buf_free(&text);
}

static size_t count_lines(span text)
{
    size_t n = 0;
    for (size_t i = 0; i < text.n; i++)
        n += text.p[i] == '\n';
    return n + (text.n && text.p[text.n - 1] != '\n');
}

static void put_patch_lines(buf *out, char sign, span text, bool last_has_newline)
{
    for (size_t i = 0; i < text.n;)
    {
        size_t n = strcspn(text.p + i, "\n");
        if (i + n > text.n)
            n = text.n - i;
        buf_putc(out, sign);
        buf_put(out, text.p + i, n);
        buf_putc(out, '\n');
        i += n + 1;
    }
    if (!last_has_newline)
        buf_puts(out, "\\ No newline at end of file\n");
}

/* A git patch for an edit that has happened: where its new text is in the file now, widened to
   whole lines, against the same lines with the old text. None when the new text is empty, or is
   in the file more than once, or every match was replaced. */
static bool put_patch(buf *patch, const char *path, const char *name, json input)
{
    buf file = {0}, old_text = {0}, new_text = {0}, before = {0};
    bool ok = false;
    json_decode(json_get(input, "old"), &old_text);
    json_decode(json_get(input, "new"), &new_text);
    if (!json_is_true(json_get(input, "all")) && new_text.n && read_file(path, &file))
    {
        char *text = buf_cstr(&file), *at = strstr(text, buf_cstr(&new_text));
        if (at && !strstr(at + 1, new_text.p) && strlen(text) == file.n)
        {
            size_t start = (size_t)(at - text), end = start + new_text.n;
            while (start && text[start - 1] != '\n')
                start--;
            if (text[end - 1] != '\n')
            {
                end += strcspn(text + end, "\n");
                if (end < file.n)
                    end++; /* through its newline */
            }
            buf_put(&before, text + start, (size_t)(at - text) - start);
            buf_putspan(&before, buf_span(&old_text));
            buf_put(&before, at + new_text.n, end - (size_t)(at - text) - new_text.n);
            span after = {text + start, end - start};
            size_t line = 1 + count_lines((span){text, start});
            bool newline = text[end - 1] == '\n';
            buf_printf(
                patch,
                "diff --git a/%s b/%s\n--- a/%s\n+++ b/%s\n@@ -%zu,%zu +%zu,%zu @@\n",
                name,
                name,
                name,
                name,
                line,
                count_lines(buf_span(&before)),
                line,
                count_lines(after)
            );
            put_patch_lines(patch, '-', buf_span(&before), newline);
            put_patch_lines(patch, '+', after, newline);
            ok = true;
        }
    }
    buf_free(&file);
    buf_free(&old_text);
    buf_free(&new_text);
    buf_free(&before);
    return ok;
}

/* An edit's change: its file, and once it is done, the patch. */
static void put_change(json_buf *items, session *s, json input, bool done)
{
    buf path = {0}, patch = {0};
    put_absolute(&path, s, json_get(input, "path"));
    size_t cwd = s->cwd ? strlen(s->cwd) : 0;
    const char *name = buf_cstr(&path);
    if (cwd && !strncmp(name, s->cwd, cwd) && name[cwd] == '/')
        name += cwd + 1;
    json_put_comma_unless_first(items);
    json_printf(
        items,
        "{'type':'diff','changes':[{'operation':'modify','path':%S,'fileType':'text'}]",
        buf_span(&path)
    );
    if (done && put_patch(&patch, buf_cstr(&path), name, input))
        json_printf(items, ",'patch':{'format':'git_patch','text':%S}", buf_span(&patch));
    buf_putc(items, '}');
    buf_free(&path);
    buf_free(&patch);
}

/* tool_call_update creating a call: its title, kind and input; a shell's terminal, a sub-agent's
   session, an edit's file. */
static void open_call2(session *s, const char *session_id, call *c, json name, json input)
{
    buf title = {0}, path = {0};
    json_buf content = {0}, extra = {0};
    put_title(&title, name, input);
    buf_putc(&content, '[');
    if (!json_is(input, '{'))
        input = S("{}");
    if (c->terminal)
    {
        json command = json_get(input, "command");
        notify(
            session_id,
            "{'sessionUpdate':'terminal_update','terminalId':%s,'command':%R,'cwd':%s,'capabilities':{'cancel':{}}}",
            c->id,
            json_is(command, '"') ? command : S("\"\""),
            s->cwd
        );
        json_printf(&content, "{'type':'terminal','terminalId':%s}", c->id);
    }
    if (c->child)
    {
        json_put_comma_unless_first(&content);
        json_printf(&content, "{'type':'session','sessionId':%s}", c->child->id);
    }
    if (json_eq(name, "edit") && json_is(json_get(input, "path"), '"'))
    {
        put_change(&content, s, input, false);
        put_absolute(&path, s, json_get(input, "path"));
        json_printf(&extra, ",'locations':[{'path':%S}]", buf_span(&path));
        buf_set(&c->input, input);
    }
    buf_putc(&content, ']');
    notify(
        session_id,
        "{'sessionUpdate':'tool_call_update','toolCallId':%s,'name':%s,'title':%S,'kind':%s,'status':'in_progress',"
        "'rawInput':%R,'content':%R%r%r}",
        c->id,
        c->name,
        buf_span(&title),
        kind_of(name),
        input,
        buf_span(&content),
        buf_cstr(&extra),
        c->pid ? ",\"capabilities\":{\"cancel\":{}}" : ""
    );
    buf_free(&title);
    buf_free(&path);
    buf_free(&content);
    buf_free(&extra);
}

static void flush_output(const char *session_id, call *c)
{
    c->dirty = false;
    c->sent_at = now_seconds();
    if (!c->tail.n)
        return;
    buf data = {0};
    buf_put_base64(&data, buf_span(&c->tail));
    notify(session_id, "{'sessionUpdate':'terminal_output_chunk','terminalId':%s,'data':%S}", c->id, buf_span(&data));
    c->tail.n = 0;
    buf_free(&data);
}

/* Whether a result ends its call cancelled: signalled by the client, or killed by an abort. */
static bool was_cancelled(const call *c, json result)
{
    jiter it = json_iter(json_get(result, "content"));
    json block;
    if (c->cancelled)
        return true;
    if (!json_next(&it, NULL, &block))
        return false;
    span text = json_get_escaped_body(json_get(block, "text"));
    return text.n >= 7 && !memcmp(text.p, "Killed:", 7);
}

static void poll_child(session *s, child *k);
static void end_child(session *s, child *k, const char *stop);

/* tool_call_update closing a call with its result: a terminal's exit, an edit's patch. */
static void close_call2(session *s, const char *session_id, call *c, json result)
{
    json content = json_get(result, "content");
    bool cancelled = was_cancelled(c, result), failed = json_is_true(json_get(result, "is_error"));
    if (c->child)
    {
        poll_child(s, c->child);
        end_child(s, c->child, cancelled ? "cancelled" : failed ? "error" : "end_turn");
    }
    json_buf items = {0}, exit = {0};
    buf_putc(&items, '[');
    if (c->terminal)
    {
        flush_output(session_id, c);
        put_exit_status(&exit, content);
        notify(
            session_id,
            "{'sessionUpdate':'terminal_update','terminalId':%s,'exitStatus':%R,'capabilities':{}}",
            c->id,
            buf_span(&exit)
        );
        json_printf(&items, "{'type':'terminal','terminalId':%s}", c->id);
    }
    if (c->child)
    {
        json_put_comma_unless_first(&items);
        json_printf(&items, "{'type':'session','sessionId':%s}", c->child->id);
    }
    if (c->input.n)
        put_change(&items, s, buf_span(&c->input), !failed);
    put_result_content(
        &items,
        content,
        c->terminal ? TEXT_NONE : !c->name || !strcmp(c->name, "shell") ? TEXT_FENCED : TEXT_PLAIN
    );
    buf_putc(&items, ']');
    notify(
        session_id,
        "{'sessionUpdate':'tool_call_update','toolCallId':%s,'status':%s,'content':%R,'rawOutput':%R%r}",
        c->id,
        cancelled ? "cancelled" : failed ? "failed" : "completed",
        buf_span(&items),
        result,
        c->pid ? ",\"capabilities\":{}" : ""
    );
    buf_free(&items);
    buf_free(&exit);
}

/* A call that will get no result: its akern is gone. */
static void abandon_call(session *s, const char *session_id, call *c)
{
    if (c->child && c->child->running)
        end_child(s, c->child, "cancelled");
    if (c->terminal)
        flush_output(session_id, c);
    notify(session_id, "{'sessionUpdate':'tool_call_update','toolCallId':%s,'status':'failed'}", c->id);
}

/* ---- protocol 2: reading a sub-agent's session file ---- */

typedef struct {
    session *s;
    child *k;
} child_ctx;

/* One entry of a sub-agent's file: its messages as the child session's, its calls as its own,
   the prompts its parent sends it as session messages. */
static void on_child_entry(void *ctx, span line)
{
    session *s = ((child_ctx *)ctx)->s;
    child *k = ((child_ctx *)ctx)->k;
    line = json_trim(line);
    if (!json_is_valid(line) || !json_eq(json_get(line, "type"), "message"))
        return;
    json id = json_get(line, "id"), message = json_get(line, "message"), content = json_get(message, "content");
    span message_id = json_get_escaped_body(id);
    if (json_eq(json_get(message, "role"), "user"))
    {
        json_buf blocks = {0};
        if (put_acp_blocks(&blocks, content, false))
            notify(
                k->id,
                "{'sessionUpdate':'session_message','messageId':%R,'senderSessionId':%s,'content':%R}",
                id,
                k->parent,
                buf_span(&blocks)
            );
        buf_free(&blocks);
        jiter it = json_iter(content);
        for (json block; json_next(&it, NULL, &block);)
        {
            json type = json_get(block, "type");
            call *c = find_in(k->calls, k->ncalls, json_get(block, "tool_use_id"));
            if ((json_eq(type, "tool_result") || json_eq(type, "job_result")) && c)
            {
                close_call2(s, k->id, c, block);
                drop_call(k->calls, &k->ncalls, c);
            }
        }
        return;
    }
    send_assistant(k->id, (span){message_id.p ? message_id.p : "", message_id.n}, content);
    jiter it = json_iter(content);
    for (json block; json_next(&it, NULL, &block);)
    {
        if (!json_eq(json_get(block, "type"), "tool_use"))
            continue;
        json name = json_get(block, "name"), input = json_get(block, "input");
        char *file = json_eq(name, "shell") ? subagent_file(json_get(input, "command")) : NULL;
        child *grandchild = file ? start_child(s, k->id, file) : NULL;
        call *c = push_call(&k->calls, &k->ncalls, &k->capcalls, json_get(block, "id"), name);
        c->child = grandchild;
        open_call2(s, k->id, c, name, input);
        free(file);
    }
}

/* Reads what a sub-agent's file has gained. */
static void poll_child(session *s, child *k)
{
    int fd = open(k->path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    child_ctx ctx = {s, k};
    char chunk[65536];
    lseek(fd, k->offset, SEEK_SET);
    for (ssize_t n; (n = read(fd, chunk, sizeof chunk)) > 0;)
    {
        k->offset += n;
        split_lines(&k->partial, (span){chunk, (size_t)n}, on_child_entry, &ctx);
    }
    close(fd);
}

static void end_child(session *s, child *k, const char *stop)
{
    if (!k->running)
        return;
    k->running = false;
    while (k->ncalls)
    {
        abandon_call(s, k->id, &k->calls[0]);
        drop_call(k->calls, &k->ncalls, &k->calls[0]);
    }
    notify(k->id, "{'sessionUpdate':'state_update','state':'idle','stopReason':%s}", stop);
    notify(
        k->parent,
        "{'sessionUpdate':'subagent_update','sessionId':%s,'capabilities':{},'state':{'state':'idle','stopReason':%s}}",
        k->id,
        stop
    );
}

static child *find_child(json id)
{
    for (size_t i = 0; i < g.nsessions; i++)
        for (size_t j = 0; j < g.sessions[i]->nchildren; j++)
            if (json_eq(id, g.sessions[i]->children[j]->id))
                return g.sessions[i]->children[j];
    return NULL;
}

/* The call that runs a top-level sub-agent, which a cancel of the child signals. */
static call *call_running(child *k)
{
    for (size_t i = 0; i < g.nsessions; i++)
        for (size_t j = 0; j < g.sessions[i]->ncalls; j++)
            if (g.sessions[i]->calls[j].child == k)
                return &g.sessions[i]->calls[j];
    return NULL;
}

/* ---- prompts ---- */

static void put_text_block(json_list *blocks, span text)
{
    json_put_comma_unless_first(blocks);
    json_printf(blocks, "{'type':'text','text':%S}", text);
}

/* ACP content blocks as akern content blocks: text, images, and resources as text. */
static void put_prompt_content(json_list *blocks, json prompt)
{
    jiter it = json_iter(prompt);
    for (json block; json_next(&it, NULL, &block);)
    {
        json type = json_get(block, "type");
        buf text = {0};
        if (json_eq(type, "text"))
        {
            json_decode(json_get(block, "text"), &text);
            put_text_block(blocks, buf_span(&text));
        }
        else if (json_eq(type, "image") && json_is(json_get(block, "data"), '"'))
        {
            json_put_comma_unless_first(blocks);
            json_printf(
                blocks,
                "{'type':'image','source':{'type':'base64','media_type':%R,'data':%R}}",
                json_get(block, "mimeType"),
                json_get(block, "data")
            );
        }
        else if (json_eq(type, "resource_link"))
        {
            buf_puts(&text, "@");
            json_decode(json_get(block, "uri"), &text);
            put_text_block(blocks, buf_span(&text));
        }
        else if (json_eq(type, "resource"))
        {
            json resource = json_get(block, "resource");
            buf_puts(&text, "<resource uri=\"");
            json_decode(json_get(resource, "uri"), &text);
            buf_puts(&text, "\">\n");
            json_decode(json_get(resource, "text"), &text);
            buf_puts(&text, "\n</resource>");
            put_text_block(blocks, buf_span(&text));
        }
        buf_free(&text);
    }
}

static void end_prompt(session *s, const char *stop, json error)
{
    if (s->prompt == PROMPT_NONE)
        return;
    json id = buf_span(&s->prompt_id);
    if (error.p)
    {
        buf message = {0};
        if (!json_decode(json_get(error, "message"), &message))
            buf_puts(&message, "akern run failed");
        json_buf line = {0};
        json_printf(
            &line,
            "{'jsonrpc':'2.0','id':%R,'error':{'code':-32603,'message':%S,'data':%R}}",
            id,
            buf_span(&message),
            error
        );
        write_line(&line);
        buf_free(&message);
    }
    else
    {
        long long *u = s->usage;
        respond(
            id,
            "{'stopReason':%s,'usage':{'inputTokens':%D,'outputTokens':%D,'cachedReadTokens':%D,"
            "'cachedWriteTokens':%D,'totalTokens':%D}}",
            stop,
            u[0],
            u[1],
            u[2],
            u[3],
            u[0] + u[1] + u[2] + u[3]
        );
    }
    s->prompt = PROMPT_NONE;
    s->prompt_id.n = 0;
    s->cancelled = s->prompt_delivered = false;
    free(s->prompt_item);
    s->prompt_item = NULL;
    memset(s->usage, 0, sizeof s->usage);
}

static void add_usage(session *s, json usage)
{
    static const char *const keys[] = {"input", "output", "cache_read", "cache_write"};
    for (size_t i = 0; i < LEN(keys); i++)
        s->usage[i] += (long long)json_get_num(json_get(usage, keys[i]), 0);
}

/* An assistant message whose text did not arrive as deltas is sent whole. */
static void note_assistant(session *s, json message)
{
    if (s->streamed)
        return;
    jiter it = json_iter(json_get(message, "content"));
    for (json block; json_next(&it, NULL, &block);)
        if (json_eq(json_get(block, "type"), "text"))
            update(
                s,
                false,
                "{'sessionUpdate':'agent_message_chunk','content':{'type':'text','text':%R}}",
                json_get(block, "text")
            );
}

/* ---- models and effort ---- */

/* The provider program for a model, as akern finds it (akern.md §2): the user's, else the
   built-in. */
static char *provider_program(const char *provider)
{
    const char *home = getenv("AKERN_HOME"), *user_home = getenv("HOME");
    static const char *const layers[] = {"/providers/", "/system/providers/"};
    for (size_t i = 0; i < LEN(layers); i++)
    {
        buf path = {0};
        if (home && *home)
            buf_printf(&path, "%s%s%s", home, layers[i], provider);
        else
            buf_printf(&path, "%s/.akern%s%s", user_home ? user_home : ".", layers[i], provider);
        if (*provider && *provider != '.' && !strchr(provider, '/') && access(buf_cstr(&path), X_OK) == 0)
            return buf_cstr(&path);
        buf_free(&path);
    }
    return NULL;
}

/* Runs `PROGRAM --models` and keeps each line's id; false, with why, when it fails. */
static bool list_models(const char *program, strv *ids, buf *why)
{
    int out[2];
    cloexec_pipe(out);
    pid_t pid = fork();
    if (pid == 0)
    {
        int null = open("/dev/null", O_RDWR);
        dup2(null, STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        dup2(null, STDERR_FILENO);
        execl(program, program, "--models", (char *)NULL);
        _exit(127);
    }
    close(out[1]);
    buf text = {0};
    double deadline = now_seconds() + MODELS_TIMEOUT_SECONDS;
    char chunk[8192];
    for (;;)
    {
        int left = (int)((deadline - now_seconds()) * 1000);
        struct pollfd fd = {out[0], POLLIN, 0};
        if (pid < 0 || left <= 0 || poll(&fd, 1, left) <= 0)
            break;
        ssize_t n = read(out[0], chunk, sizeof chunk);
        if (n <= 0)
            break;
        buf_put(&text, chunk, (size_t)n);
    }
    close(out[0]);
    /* EOF on its stdout can come a moment before the program can be waited for, so it gets
       until the deadline to exit. */
    int status = 0;
    pid_t waited = pid;
    while (pid > 0 && (waited = waitpid(pid, &status, WNOHANG)) == 0 && now_seconds() < deadline)
        poll(NULL, 0, 5);
    if (waited == 0)
        kill(pid, SIGKILL), waitpid(pid, &status, 0), buf_puts(why, "timed out");
    else if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        buf_puts(why, "failed");
    for (char *line = strtok(buf_cstr(&text), "\n"); line; line = strtok(NULL, "\n"))
    {
        json row = json_trim(S(line));
        if (!json_is_valid(row))
            continue;
        if (json_eq(json_get(row, "type"), "error"))
            why->n = 0, json_decode(json_get(row, "message"), why);
        else if (json_is(json_get(row, "id"), '"'))
            strv_push(ids, json_decode_dup(json_get(row, "id")));
    }
    buf_free(&text);
    return why->n == 0;
}

/* The models of a model's provider, asked once. */
static model_list *models_of(const char *model)
{
    size_t n = model ? strcspn(model, "/") : 0;
    char *provider = model ? span_dup((span){model, n}) : xstrdup("");
    for (size_t i = 0; i < g.nlists; i++)
        if (!strcmp(g.lists[i].provider, provider))
            return free(provider), &g.lists[i];
    model_list l = {.provider = provider};
    char *program = provider_program(provider);
    buf why = {0};
    if (!program)
        buf_printf(&why, "no provider %s", provider);
    else
        list_models(program, &l.ids, &why);
    l.error = why.n ? buf_cstr(&why) : NULL;
    free(program);
    if (g.nlists == MAX_PROVIDERS)
        return NULL;
    g.lists[g.nlists] = l;
    return &g.lists[g.nlists++];
}

/* ACP's session models: the current one, then the rest its provider lists, as provider/id. */
static void put_models(json_buf *out, const session *s)
{
    model_list *l = models_of(s->model);
    json_printf(out, "{'currentModelId':%s,'availableModels':[", s->model);
    if (s->model)
        json_printf(out, "{'modelId':%s,'name':%s}", s->model, s->model);
    for (size_t i = 0; l && i < l->ids.n; i++)
    {
        buf id = {0};
        buf_printf(&id, "%s/%s", l->provider, l->ids.v[i]);
        if (!s->model || strcmp(buf_cstr(&id), s->model))
            json_printf(out, "%r{'modelId':%S,'name':%S}", s->model || i ? "," : "", buf_span(&id), buf_span(&id));
        buf_free(&id);
    }
    buf_puts(out, "]}");
}

/* Protocol 2's model option: the current model, then the rest its provider lists. */
static void put_model_option(json_buf *out, const session *s)
{
    model_list *l = models_of(s->model);
    json_printf(
        out,
        "{'configId':'model','name':'Model','category':'model','type':'select','currentValue':%s,'options':[",
        s->model ? s->model : ""
    );
    if (s->model)
        json_printf(out, "{'value':%s,'name':%s}", s->model, s->model);
    for (size_t i = 0; l && i < l->ids.n; i++)
    {
        buf id = {0};
        buf_printf(&id, "%s/%s", l->provider, l->ids.v[i]);
        if (!s->model || strcmp(buf_cstr(&id), s->model))
            json_printf(out, "%r{'value':%S,'name':%S}", s->model || i ? "," : "", buf_span(&id), buf_span(&id));
        buf_free(&id);
    }
    buf_puts(out, "]},");
}

/* ACP's session config options: the thinking effort, a choice of the usual levels; in protocol
   2 the model first. */
static void put_config_options(json_buf *out, const session *s)
{
    bool known = !s->effort;
    buf_putc(out, '[');
    if (g.v2)
        put_model_option(out, s);
    json_printf(
        out,
        "{%s:'effort','name':'Thinking effort','description':'The effort param akern passes to its provider',"
        "'category':'thought_level','type':'select','currentValue':%s,'options':["
        "{'value':'default','name':'Default'}",
        g.v2 ? "configId" : "id",
        s->effort ? s->effort : "default"
    );
    for (size_t i = 0; i < LEN(EFFORT_LEVELS); i++)
    {
        known |= s->effort && !strcmp(s->effort, EFFORT_LEVELS[i]);
        json_printf(out, ",{'value':%s,'name':%s}", EFFORT_LEVELS[i], EFFORT_LEVELS[i]);
    }
    if (!known)
        json_printf(out, ",{'value':%s,'name':%s}", s->effort, s->effort);
    buf_puts(out, "]}]");
}

static void send_config_options(session *s)
{
    json_buf options = {0};
    put_config_options(&options, s);
    if (g.v2)
        notify(s->id, "{'sessionUpdate':'config_option_update','configOptions':%R}", buf_span(&options));
    else
        update(s, true, "{'sessionUpdate':'config_option_update','configOptions':%R}", buf_span(&options));
    buf_free(&options);
}

static void restart_akern(session *s);

/* Another model or effort is another akern on the same file, started once this one is idle. */
static void apply_model(session *s, const char *model)
{
    free(s->model);
    s->model = xstrdup(model);
    s->restart = true;
    restart_akern(s);
}

/* The session's params: {"effort": level}, or none for the provider's default. */
static void apply_effort(session *s, const char *level)
{
    bool clear = !level || !strcmp(level, "default");
    free(s->effort);
    s->effort = clear ? NULL : xstrdup(level);
    s->restart = true;
    restart_akern(s);
    if (!g.v2) /* protocol 2's callers answer with the options, or send them */
        send_config_options(s);
}

/* ---- protocol 2: state ---- */

/* The commands a prompt may be (run_command), in json_printf's quotes. */
static const char AVAILABLE_COMMANDS[] =
    "[{'type':'command','name':'model','description':'Show the model and the others its provider offers, or switch',"
    "'input':{'type':'text','hint':'provider/model'}},"
    "{'type':'command','name':'effort','description':'Show or set the thinking effort',"
    "'input':{'type':'text','hint':'low, medium, high, default, or any level the provider takes'}}]";

static const char *available_commands(void)
{
    static json_buf text;
    if (!text.n)
        json_printf(&text, AVAILABLE_COMMANDS);
    return buf_cstr(&text);
}

/* The work that outlives the run: jobs, and the sub-agents they are. */
static void put_background(json_buf *out, const session *s)
{
    buf_putc(out, '[');
    for (size_t i = 0; i < s->ncalls; i++)
    {
        const call *c = &s->calls[i];
        if (s->run_open && c->run == s->run)
            continue;
        json_put_comma_unless_first(out);
        json_printf(out, "{'type':'tool_call','toolCallId':%s}", c->id);
        if (c->child)
            json_printf(out, ",{'type':'subagent','sessionId':%s}", c->child->id);
    }
    buf_putc(out, ']');
}

/* state_update for the foreground: running, or idle with how the run ended and its usage. */
static void send_state(session *s, const char *state, const char *stop, json error)
{
    json_buf background = {0}, extra = {0};
    put_background(&background, s);
    if (stop)
    {
        long long *u = s->usage;
        json_printf(&extra, ",'stopReason':%s", stop);
        json_printf(
            &extra,
            ",'_meta':{%s:{'input':%D,'output':%D,'cache_read':%D,'cache_write':%D}}",
            META "usage",
            u[0],
            u[1],
            u[2],
            u[3]
        );
    }
    if (error.p)
    {
        buf message = {0};
        if (!json_decode(json_get(error, "message"), &message))
            buf_puts(&message, "akern run failed");
        json_printf(&extra, ",'error':{'code':-32603,'message':%S,'data':%R}", buf_span(&message), error);
        buf_free(&message);
    }
    notify(
        s->id,
        "{'sessionUpdate':'state_update','state':%s%r,'background':%R}",
        state,
        buf_cstr(&extra),
        buf_span(&background)
    );
    buf_free(&background);
    buf_free(&extra);
}

/* A title for a session from its file: its last name entry, else its first prompt's text. */
static char *title_of(span bytes)
{
    buf title = {0}, text = {0};
    bool named = false;
    for (const char *p = bytes.p, *end = bytes.p + bytes.n; p < end;)
    {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        span line = {p, (size_t)((nl ? nl : end) - p)};
        p = nl ? nl + 1 : end;
        if (!json_is_valid(line))
            continue;
        json type = json_get(line, "type"), message = json_get(line, "message");
        if (json_eq(type, "name") && json_is(json_get(line, "name"), '"'))
            text.n = 0, json_decode(json_get(line, "name"), &text), named = true;
        else if (!named && !text.n && json_eq(type, "message") && json_eq(json_get(message, "role"), "user"))
        {
            json content = json_get(message, "content");
            if (json_is(content, '"'))
                json_decode(content, &text);
            jiter it = json_iter(json_is(content, '[') ? content : S("[]"));
            for (json block; json_next(&it, NULL, &block) && !text.n;)
                if (json_eq(json_get(block, "type"), "text"))
                    json_decode(json_get(block, "text"), &text);
        }
    }
    char *start = buf_cstr(&text);
    while (isspace((unsigned char)*start))
        start++;
    buf trimmed = {0};
    buf_puts(&trimmed, start);
    put_short(&title, &trimmed);
    buf_free(&text);
    buf_free(&trimmed);
    if (!title.n)
        return buf_free(&title), NULL;
    return buf_cstr(&title);
}

/* A session takes its first prompt's text as its title. */
static void note_title(session *s, json prompt)
{
    if (s->title)
        return;
    json_buf line = {0};
    json_printf(&line, "{'type':'message','message':{'role':'user','content':%R}}", prompt);
    s->title = title_of(buf_span(&line));
    if (s->title)
        notify(s->id, "{'sessionUpdate':'session_info_update','title':%s}", s->title);
    buf_free(&line);
}

static void answer_open2(session *s)
{
    json_buf options = {0};
    put_config_options(&options, s);
    if (s->resuming)
    {
        send_state(s, "idle", NULL, NOSPAN);
        if (s->title)
            notify(s->id, "{'sessionUpdate':'session_info_update','title':%s}", s->title);
        respond(
            buf_span(&s->open_request),
            "{'configOptions':%R,'availableCommands':%r}",
            buf_span(&options),
            available_commands()
        );
    }
    else
        respond(
            buf_span(&s->open_request),
            "{'sessionId':%s,'configOptions':%R,'availableCommands':%r}",
            s->id,
            buf_span(&options),
            available_commands()
        );
    s->open_request.n = 0;
    s->resuming = false;
    buf_free(&options);
}

static pending *find_pending(session *s, json ref)
{
    for (size_t i = 0; i < s->npending; i++)
        if (json_get_num(ref, -1) == (double)s->pending[i].ref)
            return &s->pending[i];
    return NULL;
}

static pending *find_pending_item(session *s, json item)
{
    for (size_t i = 0; i < s->npending; i++)
        if (s->pending[i].item && json_eq(item, s->pending[i].item))
            return &s->pending[i];
    return NULL;
}

static void drop_pending(session *s, pending *p)
{
    free(p->item);
    buf_free(&p->request);
    buf_free(&p->prompt);
    *p = s->pending[--s->npending];
}

/* A prompt akern has put into the conversation: answered with its item's id, which is also the
   id it replays under (meta.items). */
static void prompt_inserted(session *s, pending *p)
{
    respond(buf_span(&p->request), "{'messageId':%s}", p->item);
    notify(
        s->id,
        "{'sessionUpdate':'user_message','messageId':%s,'content':%R}",
        p->item,
        buf_span(&p->prompt)
    );
    note_title(s, buf_span(&p->prompt));
    drop_pending(s, p);
}

/* Prompts whose items an abort handed back: never inserted, so refused as cancelled. */
static void prompts_returned(session *s, json items)
{
    jiter it = json_iter(items);
    for (json item; json_next(&it, NULL, &item);)
    {
        pending *p = find_pending_item(s, item);
        if (p)
        {
            respond_error(buf_span(&p->request), -32800, S("cancelled before akern took it"));
            drop_pending(s, p);
        }
    }
}

/* True for a message line holding a reply. */
static bool is_reply(json e)
{
    return json_eq(json_get(json_get(e, "message"), "role"), "assistant");
}

static void on_event2(session *s, json e)
{
    json type = json_get(e, "type");
    if (json_eq(type, "hello"))
    {
        free(s->model);
        s->model = json_decode_dup(json_get(e, "model"));
        s->ready = true;
        if (s->open_request.n)
            answer_open2(s);
    }
    else if (json_eq(type, "turn_start"))
        buf_set(&s->message_id, json_get_escaped_body(json_get(e, "id")));
    else if (json_eq(type, "text_delta") || json_eq(type, "thinking_delta"))
        notify(
            s->id,
            "{'sessionUpdate':%s,'messageId':%S,'content':{'type':'text','text':%R}}",
            json_eq(type, "text_delta") ? "agent_message_chunk" : "agent_thought_chunk",
            buf_span(&s->message_id),
            json_get(e, "text")
        );
    else if (json_eq(type, "message") && is_reply(e))
        send_assistant(s->id, buf_span(&s->message_id), json_get(json_get(e, "message"), "content"));
    else if (json_eq(type, "tool_start"))
    {
        json name = json_get(e, "name"), input = json_get(e, "input");
        pid_t pid = (pid_t)json_get_num(json_get(e, "pid"), 0);
        char *file = json_eq(name, "shell") && pid ? subagent_file(json_get(input, "command")) : NULL;
        child *k = file ? start_child(s, s->id, file) : NULL;
        call *c = push_call(&s->calls, &s->ncalls, &s->capcalls, json_get(e, "call"), name);
        c->run = (long long)json_get_num(json_get(e, "run"), 0);
        c->pid = pid;
        c->terminal = json_eq(name, "shell") && pid;
        c->child = k;
        c->sent_at = now_seconds();
        open_call2(s, s->id, c, name, input);
        free(file);
    }
    else if (json_eq(type, "tool_progress"))
    {
        call *c = find_call(s, json_get(e, "call"));
        if (c && c->terminal)
        {
            json_decode(json_get(e, "data"), &c->tail);
            c->dirty = true;
            if (c->tail.n > OUTPUT_FLUSH_BYTES)
                flush_output(s->id, c);
        }
    }
    else if (json_eq(type, "block") && json_eq(json_get(json_get(e, "block"), "type"), "tool_result"))
    {
        json block = json_get(e, "block");
        call *c = find_call(s, json_get(block, "tool_use_id"));
        if (c)
        {
            close_call2(s, s->id, c, block);
            forget_call(s, c);
        }
    }
    else if (json_eq(type, "turn_end"))
        add_usage(s, json_get(e, "usage"));
    else if (json_eq(type, "ok") && find_pending(s, json_get(e, "ref")))
        find_pending(s, json_get(e, "ref"))->item = json_decode_dup(json_get(e, "item"));
    else if (json_eq(type, "message") && json_get(json_get(e, "meta"), "items").p)
    {
        jiter it = json_iter(json_get(json_get(e, "meta"), "items"));
        for (json item; json_next(&it, NULL, &item);)
        {
            pending *p = find_pending_item(s, json_get(item, "id"));
            if (p)
                prompt_inserted(s, p);
        }
    }
    else if (json_eq(type, "returned"))
        prompts_returned(s, json_get(e, "items"));
    else if (json_eq(type, "error") && json_eq(json_get(e, "where"), "command") && find_pending(s, json_get(e, "ref")))
    {
        pending *p = find_pending(s, json_get(e, "ref"));
        buf message = {0};
        json_decode(json_get(e, "message"), &message);
        respond_error(buf_span(&p->request), -32602, buf_span(&message));
        drop_pending(s, p);
        buf_free(&message);
    }
    else if (json_eq(type, "run_start"))
    {
        s->run_open = true;
        s->run = (long long)json_get_num(json_get(e, "run"), 0);
        memset(s->usage, 0, sizeof s->usage);
        send_state(s, "running", NULL, NOSPAN);
    }
    else if (json_eq(type, "run_end"))
    {
        s->run_open = false;
        prompts_returned(s, json_get(e, "items"));
        json reason = json_get(e, "reason");
        if (json_eq(reason, "error"))
            send_state(s, "idle", "error", json_get(e, "error"));
        else
            send_state(s, "idle", json_eq(reason, "aborted") || s->cancelled ? "cancelled" : "end_turn", NOSPAN);
        s->cancelled = false;
    }
    else if (json_eq(type, "error") && json_eq(json_get(e, "where"), "command"))
        warn("session %s: akern refused a command: %.*s", s->id, (int)e.n, e.p);
}

/* True if a list of items (ids, or objects with an id) names this one. */
static bool has_item(json items, const char *id)
{
    jiter it = json_iter(items);
    for (json item; json_next(&it, NULL, &item);)
        if (json_eq(json_is(item, '{') ? json_get(item, "id") : item, id))
            return true;
    return false;
}

static void on_event(session *s, json e)
{
    json type = json_get(e, "type");
    if (g.v2)
        on_event2(s, e);
    else if (json_eq(type, "hello"))
    {
        free(s->model);
        s->model = json_decode_dup(json_get(e, "model"));
        s->ready = true;
        if (s->open_request.n)
        {
            json_buf models = {0}, options = {0};
            put_models(&models, s);
            put_config_options(&options, s);
            respond(
                buf_span(&s->open_request),
                "{'sessionId':%s,'models':%R,'configOptions':%R}",
                s->id,
                buf_span(&models),
                buf_span(&options)
            );
            s->open_request.n = 0;
            buf_free(&models);
            buf_free(&options);
        }
    }
    else if (json_eq(type, "text_delta"))
    {
        s->streamed = true;
        update(s, false, "{'sessionUpdate':'agent_message_chunk','content':{'type':'text','text':%R}}", json_get(e, "text"));
    }
    else if (json_eq(type, "thinking_delta"))
        update(s, false, "{'sessionUpdate':'agent_thought_chunk','content':{'type':'text','text':%R}}", json_get(e, "text"));
    else if (json_eq(type, "turn_start"))
        s->streamed = false;
    else if (json_eq(type, "message") && is_reply(e))
        note_assistant(s, json_get(e, "message"));
    else if (json_eq(type, "tool_start"))
        open_call(s, json_get(e, "call"), json_get(e, "name"), json_get(e, "input"), false);
    else if (json_eq(type, "tool_progress"))
        note_progress(s, e);
    else if (json_eq(type, "block") && json_eq(json_get(json_get(e, "block"), "type"), "tool_result"))
        close_call(s, json_get(e, "block"), false);
    else if (json_eq(type, "turn_end"))
        add_usage(s, json_get(e, "usage"));
    else if (json_eq(type, "ok") && s->prompt == PROMPT_SENT && json_get_num(json_get(e, "ref"), -1) == (double)s->ref)
    {
        s->prompt = PROMPT_QUEUED;
        free(s->prompt_item);
        s->prompt_item = json_decode_dup(json_get(e, "item"));
    }
    else if (
        json_eq(type, "message") && s->prompt_item &&
        has_item(json_get(json_get(e, "meta"), "items"), s->prompt_item)
    )
        s->prompt_delivered = true;
    else if (
        json_eq(type, "returned") && s->prompt == PROMPT_QUEUED && s->prompt_item &&
        has_item(json_get(e, "items"), s->prompt_item)
    )
        end_prompt(s, "cancelled", NOSPAN);
    else if (json_eq(type, "run_start"))
        s->run_open = true;
    else if (json_eq(type, "run_end"))
    {
        s->run_open = false;
        json reason = json_get(e, "reason");
        if (s->prompt == PROMPT_QUEUED && json_eq(reason, "error"))
            end_prompt(s, NULL, json_get(e, "error"));
        else if (s->prompt == PROMPT_QUEUED && (json_eq(reason, "aborted") || s->cancelled))
            end_prompt(s, "cancelled", NOSPAN);
        else if (s->prompt == PROMPT_QUEUED && !g.until_settled)
            end_prompt(s, "end_turn", NOSPAN);
    }
    else if (json_eq(type, "settled") && s->prompt == PROMPT_QUEUED)
        end_prompt(s, s->cancelled ? "cancelled" : "end_turn", NOSPAN);
    else if (json_eq(type, "error") && json_eq(json_get(e, "where"), "command"))
        warn("session %s: akern refused a command: %.*s", s->id, (int)e.n, e.p);
}

/* ---- replaying a session file for session/load ---- */

typedef struct {
    span line;
    json id;
} entry;

static bool same_string(json a, json b)
{
    span x = json_get_escaped_body(a), y = json_get_escaped_body(b);
    return json_is(a, '"') && json_is(b, '"') && x.n == y.n && !memcmp(x.p, y.p, x.n);
}

/* Protocol 2: the user messages a delivery holds, one per prompt or inject item that meta.items
   names, under the item's id as it was live, with that item's blocks. Without meta.items, one
   message under the entry's id. */
static void replay_user_messages(session *s, json content, json items, span entry_id)
{
    if (!items.p)
    {
        json_buf blocks = {0};
        if (put_acp_blocks(&blocks, content, false))
            notify(
                s->id,
                "{'sessionUpdate':'user_message','messageId':%S,'content':%R}",
                entry_id,
                buf_span(&blocks)
            );
        buf_free(&blocks);
        return;
    }
    jiter items_it = json_iter(items);
    for (json item; json_next(&items_it, NULL, &item);)
    {
        if (json_eq(json_get(item, "source"), "job"))
            continue; /* a job's result closes its call */
        double at = json_get_num(json_get(item, "at"), -1),
               n = json_get_num(json_get(item, "blocks"), 0);
        json_list own = {0};
        jiter it = json_iter(content);
        double i = 0;
        for (json block; json_next(&it, NULL, &block); i++)
            if (i >= at && i < at + n)
                json_printf(&own, "%r%R", own.n ? "," : "", block);
        json_buf array = {0}, blocks = {0};
        json_printf(&array, "[%r]", buf_cstr(&own));
        if (put_acp_blocks(&blocks, buf_span(&array), false))
            notify(
                s->id,
                "{'sessionUpdate':'user_message','messageId':%R,'content':%R}",
                json_get(item, "id"),
                buf_span(&blocks)
            );
        buf_free(&own);
        buf_free(&array);
        buf_free(&blocks);
    }
}

/* Protocol 2: a message replayed whole, by its entry's id (a user message by its items' ids);
   its calls opened and closed. */
static void replay_message2(session *s, json message, json entry_id, json meta)
{
    span id = json_get_escaped_body(entry_id);
    json content = json_get(message, "content");
    if (json_eq(json_get(message, "role"), "user"))
    {
        replay_user_messages(s, content, json_get(meta, "items"), id);
        jiter it = json_iter(content);
        for (json block; json_next(&it, NULL, &block);)
        {
            json type = json_get(block, "type");
            call *c = find_call(s, json_get(block, "tool_use_id"));
            if ((json_eq(type, "tool_result") || json_eq(type, "job_result")) && c)
            {
                close_call2(s, s->id, c, block);
                forget_call(s, c);
            }
        }
        return;
    }
    send_assistant(s->id, id, content);
    jiter it = json_iter(content);
    for (json block; json_next(&it, NULL, &block);)
        if (json_eq(json_get(block, "type"), "tool_use"))
        {
            call *c = push_call(&s->calls, &s->ncalls, &s->capcalls, json_get(block, "id"), json_get(block, "name"));
            open_call2(s, s->id, c, json_get(block, "name"), json_get(block, "input"));
        }
}

static void replay_message(session *s, json message)
{
    bool user = json_eq(json_get(message, "role"), "user");
    json content = json_get(message, "content");
    if (json_is(content, '"'))
    {
        update(s, true, "{'sessionUpdate':%s,'content':{'type':'text','text':%R}}", user ? "user_message_chunk" : "agent_message_chunk", content);
        return;
    }
    jiter it = json_iter(content);
    for (json block; json_next(&it, NULL, &block);)
    {
        json type = json_get(block, "type");
        if (json_eq(type, "text"))
            update(s, true, "{'sessionUpdate':%s,'content':{'type':'text','text':%R}}", user ? "user_message_chunk" : "agent_message_chunk", json_get(block, "text"));
        else if (json_eq(type, "thinking"))
            update(s, true, "{'sessionUpdate':'agent_thought_chunk','content':{'type':'text','text':%R}}", json_get(block, "thinking"));
        else if (json_eq(type, "tool_use"))
            open_call(s, json_get(block, "id"), json_get(block, "name"), json_get(block, "input"), true);
        else if (json_eq(type, "tool_result") || json_eq(type, "job_result"))
            close_call(s, block, true);
    }
}

/* The context: the messages after the last reset entry, oldest first; a replace entry's message
   stands in for its target's (akern.md §5). The model and effort are the last hello's: akern
   says them there each start. Without `send`, only those are read. */
static void replay_session(session *s, bool send)
{
    buf bytes = {0};
    if (!read_file(s->path, &bytes))
        return;
    free(s->title);
    s->title = title_of(buf_span(&bytes));
    entry *entries = NULL;
    size_t n = 0, cap = 0;
    for (char *p = bytes.p, *end = bytes.p + bytes.n; p < end;)
    {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl)
            break;
        span line = {p, (size_t)(nl - p)};
        p = nl + 1;
        if (!json_is_valid(line))
            continue;
        if (json_eq(json_get(line, "type"), "hello"))
        {
            free(s->model), free(s->effort);
            s->model = json_decode_dup(json_get(line, "model"));
            s->effort = json_decode_dup(json_get(json_get(line, "params"), "effort"));
        }
        entry e = {line, json_get(line, "id")};
        if (!e.id.p)
            continue;
        /* a message sits where the first line with its id is: its reply's turn_start */
        bool placed = false;
        for (size_t i = n; json_eq(json_get(line, "type"), "message") && !placed && i-- > 0;)
            if (same_string(entries[i].id, e.id) &&
                json_eq(json_get(entries[i].line, "type"), "turn_start"))
                entries[i] = e, placed = true;
        if (!placed)
            VEC_PUSH(entries, n, cap, e);
    }
    size_t context_start = 0;
    for (size_t i = 0; i < n; i++)
        if (json_eq(json_get(entries[i].line, "type"), "reset"))
            context_start = i + 1;
    /* replace entries, read once: the last one for a target wins */
    entry *replaces = NULL;
    size_t nreplaces = 0, capreplaces = 0;
    for (size_t i = 0; i < n; i++)
        if (json_eq(json_get(entries[i].line, "type"), "replace") &&
            json_is(json_get(entries[i].line, "message"), '{'))
        {
            entry r = {json_get(entries[i].line, "message"), json_get(entries[i].line, "target")};
            VEC_PUSH(replaces, nreplaces, capreplaces, r);
        }
    for (size_t i = 0; i < n; i++)
    {
        entry *e = &entries[i];

        if (!send || i < context_start || !json_eq(json_get(e->line, "type"), "message"))
            continue;
        json message = json_get(e->line, "message");
        for (size_t r = nreplaces; r-- > 0;)
            if (same_string(replaces[r].id, e->id))
            {
                message = replaces[r].line;
                break;
            }
        if (g.v2)
            replay_message2(s, message, e->id, json_get(e->line, "meta"));
        else
            replay_message(s, message);
    }
    free(replaces);
    free(entries);
    buf_free(&bytes);
}

/* ---- requests ---- */

static void warn_mcp(json params)
{
    jiter it = json_iter(json_get(params, "mcpServers"));
    size_t n = 0;
    for (json server; json_next(&it, NULL, &server);)
        n++;
    if (n)
        warn("akern has no MCP client; ignoring %zu MCP server(s)", n);
}

static void start_session(json id, json params, const char *session_id, bool load, bool replay)
{
    session *s = add_session(session_id, json_get(params, "cwd"));
    if (!s)
    {
        respond_error(id, -32603, S("too many sessions"));
        return;
    }
    warn_mcp(params);
    if (load)
        replay_session(s, replay);
    if (g.v2 && load)
    {
        /* calls the file leaves open died with the akern that ran them */
        while (s->ncalls)
        {
            abandon_call(s, s->id, &s->calls[0]);
            forget_call(s, &s->calls[0]);
        }
        if (replay)
            notify(s->id, "{'sessionUpdate':'replay_complete'}");
        s->resuming = true;
    }
    buf_set(&s->open_request, id);
    if (!spawn_akern(s))
        respond_error(id, -32603, S("cannot start akern")), s->open_request.n = 0;
}

static void new_session(json id, json params)
{
    char *wanted = json_decode_dup(json_get(params, "sessionId"));
    char *session_id = wanted && is_safe_id(wanted) && !find_session(json_get(params, "sessionId")) ? wanted : new_id();
    if (session_id != wanted)
        free(wanted);
    start_session(id, params, session_id, false, false);
    free(session_id);
}

/* session/load, and protocol 2's session/resume, which replays only when asked. */
static void load_session(json id, json params, bool resume)
{
    json from = json_get(params, "replayFrom");
    bool replay = !resume || json_eq(json_get(from, "type"), "start");
    if (resume && !replay && !json_is_null_or_absent(from))
    {
        respond_error(id, -32602, S("replayFrom must be {\"type\":\"start\"} or absent"));
        return;
    }
    char *session_id = json_decode_dup(json_get(params, "sessionId"));
    buf path = {0};
    if (session_id && is_safe_id(session_id))
        buf_printf(&path, "%s/%s.jsonl", g.sessions_dir, session_id);
    if (!path.n || access(buf_cstr(&path), F_OK) != 0)
        respond_error(id, -32002, S("no such session"));
    else if (find_session(json_get(params, "sessionId")))
        respond_error(id, -32602, S("the session is already open"));
    else
        start_session(id, params, session_id, true, replay);
    free(session_id);
    buf_free(&path);
}

/* "/model [ID]" or "/effort [LEVEL]" alone: the name and the argument, if any. */
static bool parse_command(const char *text, span *name, span *arg)
{
    while (isspace((unsigned char)*text))
        text++;
    if (*text++ != '/')
        return false;
    *name = (span){text, strcspn(text, " \t\r\n")};
    text += name->n;
    while (isspace((unsigned char)*text))
        text++;
    *arg = (span){text, strcspn(text, " \t\r\n")};
    text += arg->n;
    while (isspace((unsigned char)*text))
        text++;
    return !*text && (span_eq(*name, "model") || span_eq(*name, "effort"));
}

/* A prompt that is only /model or /effort is answered here; it never reaches the model, since
   clients such as Omnigent have no control of their own for an ACP agent's model or effort. */
static bool run_command(session *s, json id, json prompt)
{
    jiter it = json_iter(prompt);
    json block, second;
    if (!json_next(&it, NULL, &block) || json_next(&it, NULL, &second) ||
        !json_eq(json_get(block, "type"), "text"))
        return false;
    buf text = {0}, reply = {0};
    json_decode(json_get(block, "text"), &text);
    span name, arg;
    if (!parse_command(buf_cstr(&text), &name, &arg))
    {
        buf_free(&text);
        return false;
    }
    char *value = arg.n ? span_dup(arg) : NULL;
    if (span_eq(name, "model") && !value)
    {
        model_list *l = models_of(s->model);
        buf_printf(&reply, "Model: %s\n", s->model ? s->model : "none");
        if (l && l->error)
            buf_printf(&reply, "\n(could not list %s's models: %s)\n", l->provider, l->error);
        else if (l)
        {
            buf_printf(&reply, "\n%s offers:\n", l->provider);
            for (size_t i = 0; i < l->ids.n; i++)
                buf_printf(&reply, "- %s/%s\n", l->provider, l->ids.v[i]);
        }
        buf_puts(&reply, "\nSwitch with /model <id> or /model <provider>/<id>.");
    }
    else if (span_eq(name, "model"))
    {
        buf model = {0};
        if (strchr(value, '/'))
            buf_puts(&model, value);
        else
            buf_printf(&model, "%.*s/%s", (int)(s->model ? strcspn(s->model, "/") : 0), s->model ? s->model : "", value);
        apply_model(s, buf_cstr(&model));
        buf_printf(&reply, "Model: %s", s->model);
        buf_free(&model);
    }
    else
    {
        if (value)
            apply_effort(s, value);
        buf_printf(&reply, "Effort: %s", s->effort ? s->effort : "the provider default");
        if (!value)
            buf_puts(&reply, ". Set it with /effort <level> or /effort default.");
    }
    if (g.v2)
    {
        /* a command handled here runs from idle to idle; its prompt is a live-only message */
        buf message_id = {0};
        char *unique = new_id();
        buf_printf(&message_id, "command-%s", unique); /* never an akern id, which is 8 hex */
        free(unique);
        respond(id, "{'messageId':%S}", buf_span(&message_id));
        notify(s->id, "{'sessionUpdate':'user_message','messageId':%S,'content':%R}", buf_span(&message_id), prompt);
        bool idle = !s->run_open;
        if (idle)
            send_state(s, "running", NULL, NOSPAN);
        if (value)
            send_config_options(s);
        buf_puts(&message_id, "-reply");
        notify(
            s->id,
            "{'sessionUpdate':'agent_message','messageId':%S,'content':[{'type':'text','text':%S}]}",
            buf_span(&message_id),
            buf_span(&reply)
        );
        if (idle)
        {
            long long usage[4];
            memcpy(usage, s->usage, sizeof usage);
            memset(s->usage, 0, sizeof s->usage);
            send_state(s, "idle", "end_turn", NOSPAN);
            memcpy(s->usage, usage, sizeof usage);
        }
        buf_free(&message_id);
    }
    else
    {
        update(s, true, "{'sessionUpdate':'agent_message_chunk','content':{'type':'text','text':%S}}", buf_span(&reply));
        respond(id, "{'stopReason':'end_turn'}");
    }
    free(value);
    buf_free(&text);
    buf_free(&reply);
    return true;
}

static void prompt(json id, json params)
{
    session *s = find_session(json_get(params, "sessionId"));
    if (!s)
    {
        respond_error(id, -32002, S("no such session"));
        return;
    }
    if (s->prompt != PROMPT_NONE && !g.v2)
    {
        respond_error(id, -32602, S("a prompt is already running in this session"));
        return;
    }
    if (s->exited && !spawn_akern(s))
    {
        respond_error(id, -32603, S("cannot start akern"));
        return;
    }
    flush_held(s);
    if (run_command(s, id, json_get(params, "prompt")))
        return;
    s->settled = false; /* until akern says so: a switch now waits for this prompt's work */
    json_list blocks = {0};
    put_prompt_content(&blocks, json_get(params, "prompt"));
    if (!blocks.n)
        put_text_block(&blocks, S(""));
    if (g.v2)
    {
        /* any time: it steers a run at its next turn, or starts one; `now` preempts the turn */
        json when = json_get(json_get(params, "_meta"), META "when");
        if (!json_is_null_or_absent(when) && !json_eq(when, "now") && !json_eq(when, "turn") &&
            !json_eq(when, "idle") && !json_eq(when, "deferred"))
            respond_error(id, -32602, S("_meta." META "when must be now, turn, idle or deferred"));
        else
        {
            json prompt = json_get(params, "prompt");
            pending p = {.ref = ++s->ref};
            buf_set(&p.request, id);
            buf_set(&p.prompt, json_is(prompt, '[') ? prompt : S("[]"));
            VEC_PUSH(s->pending, s->npending, s->cappending, p);
            send_to_child(
                s,
                "{'type':'send','content':[%r],'when':%R,'ref':%D}",
                buf_cstr(&blocks),
                json_is(when, '"') ? when : S("\"turn\""),
                p.ref
            );
        }
        buf_free(&blocks);
        return;
    }
    buf_set(&s->prompt_id, id);
    s->prompt = PROMPT_SENT;
    s->updated_at = now_seconds();
    s->cancelled = false;
    memset(s->usage, 0, sizeof s->usage);
    send_to_child(s, "{'type':'send','content':[%r],'ref':%D}", buf_cstr(&blocks), ++s->ref);
    buf_free(&blocks);
}

/* Protocol 2: the run, and a prompt akern has not yet taken; jobs keep running. A child's
   cancel signals the call that runs it. */
static void cancel2(json params)
{
    session *s = find_session(json_get(params, "sessionId"));
    child *k = s ? NULL : find_child(json_get(params, "sessionId"));
    call *c = k ? call_running(k) : NULL;
    if (c)
        cancel_call(c);
    if (!s || (!s->run_open && !s->npending))
        return;
    /* akern hands back what it has not delivered, and prompts_returned refuses those */
    s->cancelled = true;
    send_to_child(s, "{'type':'abort'}");
}

/* tool_call/cancel and terminal/cancel: the call's process group gets SIGTERM. */
static void cancel_item(json id, json params, const char *key)
{
    session *s = find_session(json_get(params, "sessionId"));
    call *c = s ? find_call(s, json_get(params, key)) : NULL;
    if (!s)
        respond_error(id, -32002, S("no such session"));
    else if (!c || (!strcmp(key, "terminalId") && !c->terminal) || !cancel_call(c))
        respond_error(id, -32602, S("no running call by that id that can be cancelled"));
    else
        respond(id, "{}");
}

/* Ends akern with its jobs; answered once it has exited (child_ended). */
static void close_session(json id, json params)
{
    session *s = find_session(json_get(params, "sessionId"));
    if (!s && find_child(json_get(params, "sessionId")))
        respond_error(id, -32602, S("a child session closes with its parent"));
    else if (!s || s->closing)
        respond(id, "{}");
    else if (s->exited)
        respond(id, "{}"), s->closing = true;
    else
    {
        buf_set(&s->close_request, id);
        s->cancelled = s->run_open;
        send_to_child(s, "{'type':'abort','jobs':true}");
        close_fd(&s->to_child);
    }
}

static void put_rfc3339(buf *out, time_t t)
{
    struct tm tm;
    char text[32];
    gmtime_r(&t, &tm);
    strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &tm);
    buf_puts(out, text);
}

/* The session files in the sessions directory, by the cwd in each one's header. */
static void list_sessions(json id, json params)
{
    char *cwd = json_decode_dup(json_get(params, "cwd"));
    json_buf sessions = {0};
    buf_putc(&sessions, '[');
    DIR *dir = opendir(g.sessions_dir);
    for (struct dirent *d; dir && (d = readdir(dir));)
    {
        size_t n = strlen(d->d_name);
        char *session_id = n > 6 && !strcmp(d->d_name + n - 6, ".jsonl") ? span_dup((span){d->d_name, n - 6}) : NULL;
        buf path = {0}, bytes = {0}, updated = {0};
        struct stat st;
        if (session_id && is_safe_id(session_id))
            buf_printf(&path, "%s/%s", g.sessions_dir, d->d_name);
        if (path.n && stat(buf_cstr(&path), &st) == 0 && read_file(buf_cstr(&path), &bytes))
        {
            span header = {bytes.p, strcspn(buf_cstr(&bytes), "\n")};
            json at = json_get(header, "cwd");
            char *where = json_is_valid(header) && json_eq(json_get(header, "type"), "session") ? json_decode_dup(at) : NULL;
            if (where && (!cwd || !strcmp(cwd, where)))
            {
                char *title = title_of(buf_span(&bytes));
                put_rfc3339(&updated, st.st_mtime);
                json_put_comma_unless_first(&sessions);
                json_printf(&sessions, "{'sessionId':%s,'cwd':%s", session_id, where);
                if (title)
                    json_printf(&sessions, ",'title':%s", title);
                json_printf(&sessions, ",'updatedAt':%S}", buf_span(&updated));
                free(title);
            }
            free(where);
        }
        free(session_id);
        buf_free(&path);
        buf_free(&bytes);
        buf_free(&updated);
    }
    if (dir)
        closedir(dir);
    buf_putc(&sessions, ']');
    respond(id, "{'sessions':%R}", buf_span(&sessions));
    buf_free(&sessions);
    free(cwd);
}

static void cancel(json params)
{
    session *s = find_session(json_get(params, "sessionId"));
    if (g.v2)
    {
        cancel2(params);
        return;
    }
    if (!s || s->prompt == PROMPT_NONE)
        return;
    s->cancelled = true;
    if (s->prompt == PROMPT_QUEUED && s->prompt_delivered && !s->run_open)
        end_prompt(s, "cancelled", NOSPAN); /* only jobs are left: they keep running */
    else
        send_to_child(s, "{'type':'abort'}"); /* ends the run, or hands the prompt back */
}

static void set_model(json id, json params, json model)
{
    session *s = find_session(json_get(params, "sessionId"));
    if (!s || !json_is(model, '"'))
    {
        respond_error(id, -32602, S(s ? "no model" : "no such session"));
        return;
    }
    char *name = json_decode_dup(model);
    apply_model(s, name);
    free(name);
    respond(id, "{}");
}

static void set_config_option(json id, json params)
{
    session *s = find_session(json_get(params, "sessionId"));
    json config = json_get(params, "configId"), value = json_get(params, "value");
    json type = json_get(params, "type");
    if (g.v2 && s && json_eq(config, "model") && json_is(value, '"') && (json_is_null_or_absent(type) || json_eq(type, "id")))
    {
        char *name = json_decode_dup(value);
        apply_model(s, name);
        free(name);
        json_buf options = {0};
        put_config_options(&options, s);
        respond(id, "{'configOptions':%R}", buf_span(&options));
        buf_free(&options);
        return;
    }
    if (json_eq(config, "model"))
    {
        set_model(id, params, value);
        return;
    }
    if (g.v2 && !json_is_null_or_absent(type) && !json_eq(type, "id"))
    {
        respond_error(id, -32602, S("effort is a select: type must be id"));
        return;
    }
    if (!s || !json_eq(config, "effort") || !json_is(value, '"'))
    {
        respond_error(id, -32602, S(s ? "unknown config option" : "no such session"));
        return;
    }
    char *level = json_decode_dup(value);
    apply_effort(s, level);
    free(level);
    json_buf options = {0};
    put_config_options(&options, s);
    respond(id, "{'configOptions':%R}", buf_span(&options));
    buf_free(&options);
}

static void on_request(json msg)
{
    json method = json_get(msg, "method"), id = json_get(msg, "id"), params = json_get(msg, "params");
    bool request = id.p != NULL;
    double version = json_get_num(json_get(params, "protocolVersion"), 1);
    if (json_eq(method, "initialize") && version >= 2)
    {
        g.v2 = true;
        respond(
            id,
            "{'protocolVersion':2,'info':{'name':'akern-acp','title':'akern','version':'0.2.0'},"
            "'capabilities':{'session':{'prompt':{'image':{},'embeddedContext':{}}}},'authMethods':[]}"
        );
    }
    else if (json_eq(method, "initialize"))
        g.v2 = false, respond(
            id,
            "{'protocolVersion':1,'agentCapabilities':{'loadSession':true,'promptCapabilities':{"
            "'image':true,'audio':false,'embeddedContext':true},'mcpCapabilities':{'http':false,"
            "'sse':false}},'authMethods':[],'agentInfo':{'name':'akern-acp','version':'0.1.0'}}"
        );
    else if (json_eq(method, "authenticate"))
        respond(id, "{}");
    else if (json_eq(method, "session/new"))
        new_session(id, params);
    else if (json_eq(method, "session/load"))
        load_session(id, params, false);
    else if (g.v2 && json_eq(method, "session/resume"))
        load_session(id, params, true);
    else if (g.v2 && json_eq(method, "session/list"))
        list_sessions(id, params);
    else if (g.v2 && json_eq(method, "session/close"))
        close_session(id, params);
    else if (g.v2 && json_eq(method, "tool_call/cancel"))
        cancel_item(id, params, "toolCallId");
    else if (g.v2 && json_eq(method, "terminal/cancel"))
        cancel_item(id, params, "terminalId");
    else if (json_eq(method, "session/prompt"))
        prompt(id, params);
    else if (json_eq(method, "session/cancel"))
        cancel(params);
    else if (json_eq(method, "session/set_model"))
        set_model(id, params, json_get(params, "modelId"));
    else if (json_eq(method, "session/set_config_option"))
        set_config_option(id, params);
    else if (request && json_is(method, '"'))
        respond_error(id, -32601, S("method not found"));
}

/* ---- reading lines ---- */

static void split_lines(buf *partial, span chunk, void (*fn)(void *, span), void *ctx)
{
    buf_putspan(partial, chunk);
    size_t start = 0;
    for (size_t i = 0; i < partial->n; i++)
        if (partial->p[i] == '\n')
        {
            fn(ctx, (span){partial->p + start, i - start});
            start = i + 1;
        }
    buf_drop_front(partial, start);
}

static void on_client_line(void *ctx, span line)
{
    (void)ctx;
    line = json_trim(line);
    if (!line.n)
        return;
    if (json_is_valid(line) && json_is(line, '[') && g.v2)
    {
        respond_error(S("null"), -32600, S("batches are not supported"));
        return;
    }
    if (!json_is_valid(line) || !json_is(line, '{'))
    {
        respond_error(S("null"), -32700, S("parse error"));
        return;
    }
    on_request(line);
}

static void on_child_line(void *ctx, span line)
{
    session *s = ctx;
    if (!json_is_valid(line))
        return;
    json e = json_trim(line), type = json_get(e, "type");
    if (json_eq(type, "queued") || json_eq(type, "run_start"))
        s->settled = false;
    else if (json_eq(type, "settled"))
        s->settled = true;
    on_event(s, e);
}

static void on_child_err_line(void *ctx, span line)
{
    session *s = ctx;
    fprintf(stderr, "akern[%s]: %.*s\n", s->id, (int)line.n, line.p);
    buf_putspan(&s->err_text, line);
    buf_putc(&s->err_text, '\n');
}

static void close_fd(int *fd)
{
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
}

static bool read_into(int *fd, buf *partial, void (*fn)(void *, span), void *ctx);

/* akern has exited: what it said on stderr is read first, since that is the reason a client
   gets. A descendant that kept stderr open is not waited for. */
static void child_ended(session *s)
{
    int status = 0;
    waitpid(s->pid, &status, 0);
    close_fd(&s->to_child);
    if (s->child_err >= 0)
    {
        fcntl(s->child_err, F_SETFL, fcntl(s->child_err, F_GETFL) | O_NONBLOCK);
        while (s->child_err >= 0 && read_into(&s->child_err, &s->err_partial, on_child_err_line, s))
            if (errno == EAGAIN)
                break;
        if (s->err_partial.n)
            split_lines(&s->err_partial, S("\n"), on_child_err_line, s);
    }
    close_fd(&s->child_err);
    bool was_running = s->run_open;
    s->exited = true;
    s->run_open = false;
    span why = s->err_text.n ? buf_span(&s->err_text) : S("akern exited");
    if (s->open_request.n)
        respond_error(buf_span(&s->open_request), -32603, why), s->open_request.n = 0;
    if (g.v2)
    {
        bool closing = s->close_request.n;
        while (s->npending)
        {
            pending *p = &s->pending[0];
            respond_error(buf_span(&p->request), closing ? -32800 : -32603, closing ? S("the session closed") : why);
            drop_pending(s, p);
        }
        while (s->ncalls)
        {
            abandon_call(s, s->id, &s->calls[0]);
            forget_call(s, &s->calls[0]);
        }
        for (size_t i = 0; i < s->nchildren; i++)
            end_child(s, s->children[i], "cancelled");
        if (was_running)
        {
            json_buf error = {0};
            json_printf(&error, "{'where':'kernel','message':%S}", why);
            if (closing)
                send_state(s, "idle", "cancelled", NOSPAN);
            else
                send_state(s, "idle", "error", buf_span(&error));
            buf_free(&error);
        }
        if (closing)
            respond(buf_span(&s->close_request), "{}"), s->closing = true;
        return;
    }
    if (s->prompt != PROMPT_NONE)
    {
        json_buf error = {0};
        json_printf(&error, "{'where':'kernel','message':%S}", why);
        end_prompt(s, NULL, buf_span(&error));
        buf_free(&error);
    }
    while (s->ncalls)
        forget_call(s, &s->calls[0]);
}

static bool read_into(int *fd, buf *partial, void (*fn)(void *, span), void *ctx)
{
    char chunk[65536];
    errno = 0;
    ssize_t n = read(*fd, chunk, sizeof chunk);
    if (n < 0 && (errno == EINTR || errno == EAGAIN))
        return true;
    if (n <= 0)
    {
        if (partial->n)
            split_lines(partial, S("\n"), fn, ctx);
        close_fd(fd);
        return false;
    }
    split_lines(partial, (span){chunk, (size_t)n}, fn, ctx);
    return true;
}

/* The akern of a session, told EOF once it is idle so that it exits, and started again: a new
   model or effort takes a new start (akern.md §2). */
static void restart_akern(session *s)
{
    if (s->open_request.n || !s->settled || s->exited || s->to_child < 0)
        return; /* when it is: once settled, or the next prompt's start */
    s->restart = false;
    close_fd(&s->to_child);
    fcntl(s->from_child, F_SETFL, fcntl(s->from_child, F_GETFL) & ~O_NONBLOCK);
    char *model = s->model, *effort = s->effort; /* the choice, which the old hello would undo */
    s->model = s->effort = NULL;
    while (s->from_child >= 0 && read_into(&s->from_child, &s->out_partial, on_child_line, s))
        ;
    child_ended(s);
    free(s->model), free(s->effort);
    s->model = model, s->effort = effort;
    spawn_akern(s);
}

/* ---- the loop ---- */

static bool needs_keepalive(const session *s)
{
    return !g.v2 && s->prompt != PROMPT_NONE && s->ncalls;
}

static bool any_running_child(void)
{
    for (size_t i = 0; i < g.nsessions; i++)
        for (size_t j = 0; j < g.sessions[i]->nchildren; j++)
            if (g.sessions[i]->children[j]->running)
                return true;
    return false;
}

static int progress_timeout_ms(void)
{
    double soonest = -1, t = now_seconds();
    for (size_t i = 0; i < g.nsessions; i++)
        if (needs_keepalive(g.sessions[i]))
        {
            double wait = g.sessions[i]->updated_at + KEEPALIVE_SECONDS - t;
            if (soonest < 0 || wait < soonest)
                soonest = wait < 0 ? 0 : wait;
        }
    for (size_t i = 0; i < g.nsessions; i++)
        for (size_t j = 0; j < g.sessions[i]->ncalls; j++)
        {
            call *c = &g.sessions[i]->calls[j];
            if (!c->dirty)
                continue;
            double wait = c->sent_at + PROGRESS_EVERY_SECONDS - t;
            if (soonest < 0 || wait < soonest)
                soonest = wait < 0 ? 0 : wait;
        }
    if (any_running_child())
    {
        double wait = g.polled_at + CHILD_POLL_SECONDS - t;
        if (soonest < 0 || wait < soonest)
            soonest = wait < 0 ? 0 : wait;
    }
    return soonest < 0 ? -1 : (int)ceil(soonest * 1000);
}

static void send_due_progress(void)
{
    double t = now_seconds();
    for (size_t i = 0; i < g.nsessions; i++)
    {
        session *s = g.sessions[i];
        if (needs_keepalive(s) && t >= s->updated_at + KEEPALIVE_SECONDS)
        {
            json_buf id = {0};
            json_put_string(&id, S(s->calls[0].id));
            update(s, false, "{'sessionUpdate':'tool_call_update','toolCallId':%R,'status':'in_progress'}", buf_span(&id));
            buf_free(&id);
        }
    }
    for (size_t i = 0; i < g.nsessions; i++)
        for (size_t j = 0; j < g.sessions[i]->ncalls; j++)
        {
            call *c = &g.sessions[i]->calls[j];
            if (c->dirty && t >= c->sent_at + PROGRESS_EVERY_SECONDS && c->terminal)
                flush_output(g.sessions[i]->id, c);
            else if (c->dirty && t >= c->sent_at + PROGRESS_EVERY_SECONDS)
                send_progress(g.sessions[i], c);
        }
    if (t < g.polled_at + CHILD_POLL_SECONDS)
        return;
    g.polled_at = t;
    for (size_t i = 0; i < g.nsessions; i++)
        for (size_t j = 0; j < g.sessions[i]->nchildren; j++) /* a child read may add children */
            if (g.sessions[i]->children[j]->running)
                poll_child(g.sessions[i], g.sessions[i]->children[j]);
}

static void free_child(child *k)
{
    while (k->ncalls)
        drop_call(k->calls, &k->ncalls, &k->calls[0]);
    free(k->calls);
    free(k->id), free(k->parent), free(k->path), free(k->title);
    buf_free(&k->partial);
    free(k);
}

/* A closed session is forgotten, once nothing refers to it. */
static void remove_closed(void)
{
    for (size_t i = 0; i < g.nsessions;)
    {
        session *s = g.sessions[i];
        if (!s->closing || s->from_child >= 0 || s->child_err >= 0)
        {
            i++;
            continue;
        }
        close_fd(&s->to_child);
        while (s->ncalls)
            forget_call(s, &s->calls[0]);
        while (s->npending)
            drop_pending(s, &s->pending[0]);
        free(s->prompt_item);
        for (size_t j = 0; j < s->nchildren; j++)
            free_child(s->children[j]);
        free(s->children), free(s->pending), free(s->calls);
        free(s->id), free(s->cwd), free(s->path), free(s->model), free(s->effort), free(s->title);
        buf_free(&s->out_partial), buf_free(&s->err_partial), buf_free(&s->err_text), buf_free(&s->held);
        buf_free(&s->open_request), buf_free(&s->prompt_id), buf_free(&s->message_id), buf_free(&s->close_request);
        free(s);
        g.sessions[i] = g.sessions[--g.nsessions];
    }
}

static bool any_child(void)
{
    for (size_t i = 0; i < g.nsessions; i++)
        if (g.sessions[i]->from_child >= 0)
            return true;
    return false;
}

/* The client is gone: each akern is told to stop its jobs too, or a job would keep it (and its
   session's lock) alive with nobody to report to. */
static void close_children_stdin(void)
{
    for (size_t i = 0; i < g.nsessions; i++)
    {
        session *s = g.sessions[i];
        if (s->to_child >= 0)
            send_to_child(s, "{'type':'abort','jobs':true}");
        close_fd(&s->to_child);
    }
}

static void run(void)
{
    while (!g.stdin_closed || any_child())
    {
        struct pollfd fds[1 + 2 * MAX_SESSIONS];
        session *owners[1 + 2 * MAX_SESSIONS];
        bool is_err[1 + 2 * MAX_SESSIONS];
        nfds_t n = 0;
        if (!g.stdin_closed)
            fds[n] = (struct pollfd){STDIN_FILENO, POLLIN, 0}, owners[n] = NULL, is_err[n++] = false;
        for (size_t i = 0; i < g.nsessions; i++)
        {
            session *s = g.sessions[i];
            /* stderr first, so its last lines are read before stdout's end closes it */
            if (s->child_err >= 0)
                fds[n] = (struct pollfd){s->child_err, POLLIN, 0}, owners[n] = s, is_err[n++] = true;
            if (s->from_child >= 0)
                fds[n] = (struct pollfd){s->from_child, POLLIN, 0}, owners[n] = s, is_err[n++] = false;
        }
        if (poll(fds, n, progress_timeout_ms()) < 0 && errno != EINTR)
            die("poll: %s", strerror(errno));
        for (nfds_t i = 0; i < n; i++)
        {
            if (!fds[i].revents)
                continue;
            session *s = owners[i];
            if (!s)
            {
                if (!read_into(&(int){STDIN_FILENO}, &g.in_partial, on_client_line, NULL))
                    g.stdin_closed = true, close_children_stdin();
            }
            else if (is_err[i])
            {
                if (fds[i].fd == s->child_err)
                    read_into(&s->child_err, &s->err_partial, on_child_err_line, s);
            }
            else if (fds[i].fd == s->from_child &&
                     !read_into(&s->from_child, &s->out_partial, on_child_line, s))
                child_ended(s);
        }
        send_due_progress();
        for (size_t i = 0; i < g.nsessions; i++)
            if (g.sessions[i]->restart)
                restart_akern(g.sessions[i]);
        remove_closed();
    }
}

static void usage(void)
{
    fputs(
        "usage: akern-acp [--until run|settled] [--background hold|send] [--sessions DIR] "
        "[-- akern args...]\n",
        stderr
    );
    exit(2);
}

static char *default_sessions_dir(void)
{
    const char *dir = getenv("AKERN_ACP_SESSIONS"), *state = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    buf path = {0};
    if (dir && *dir)
        buf_puts(&path, dir);
    else if (state && *state)
        buf_printf(&path, "%s/akern/acp", state);
    else
        buf_printf(&path, "%s/.local/state/akern/acp", home ? home : ".");
    return buf_cstr(&path);
}

/* AKERN_MODEL, else the first line of $AKERN_HOME/model, as akern-cli does (tools.md §4). */
static void default_model(void)
{
    const char *model = getenv("AKERN_MODEL"), *home = getenv("AKERN_HOME");
    if (model && *model)
        return;
    buf path = {0}, text = {0};
    if (home && *home)
        buf_printf(&path, "%s/model", home);
    else
        buf_printf(&path, "%s/.akern/model", getenv("HOME") ? getenv("HOME") : ".");
    if (read_file(buf_cstr(&path), &text) && text.n)
    {
        text.n = strcspn(buf_cstr(&text), "\r\n");
        if (text.n)
            setenv("AKERN_MODEL", buf_cstr(&text), 1);
    }
    buf_free(&path);
    buf_free(&text);
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc; i++)
    {
        const char *a = argv[i];
        if (!strcmp(a, "--"))
        {
            i++;
            break;
        }
        if (!strcmp(a, "--help") || i + 1 >= argc)
            usage();
        const char *v = argv[++i];
        if (!strcmp(a, "--until") && (!strcmp(v, "run") || !strcmp(v, "settled")))
            g.until_settled = !strcmp(v, "settled");
        else if (!strcmp(a, "--background") && (!strcmp(v, "hold") || !strcmp(v, "send")))
            g.send_background = !strcmp(v, "send");
        else if (!strcmp(a, "--sessions"))
            g.sessions_dir = xstrdup(v);
        else
            usage();
    }
    for (; i < argc; i++)
        strv_push(&g.akern_args, xstrdup(argv[i]));
    if (!g.sessions_dir)
        g.sessions_dir = default_sessions_dir();
    make_dirs(g.sessions_dir);
    default_model();
    signal(SIGPIPE, SIG_IGN);
    run();
    return 0;
}
