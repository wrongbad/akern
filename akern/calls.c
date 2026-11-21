#include "calls.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "inbox.h"
#include "loop.h"
#include "message.h"
#include "output.h"
#include "proc.h"
#include "tools.h"

enum {
    DEFAULT_OUTPUT_LIMIT_BYTES = 100000,
    MAX_SHOWN_INPUT_BYTES = 200,
    SNIFF_BYTES = 12,
    BINARY_SNIFF_BYTES = 8192
};
static const double MAX_OUTPUT_LIMIT_BYTES = 1e12;

typedef enum { CALL_RUNNING, CALL_DONE, CALL_CANCELLED } call_state;

static const char *const state_names[] = {"running", "done", "cancelled"};

typedef struct {
    buf kept;
    size_t total_bytes, limit_bytes;
    int file_fd;
    char *file_path, *file_error;
} capture;

typedef struct call {
    calls *calls;
    char *id, *tool_name;
    json_buf input;
    int run, turn;
    call_state state;
    bool in_turn, wait, delivers_late, killed, overflowed, announced;

    worker *proc;
    char *program, *media_type, *media_name;
    char *target;    /* a job call: the job it waits for */
    double deadline; /* when a job call's or a shell call's wait runs out; 0 for none */
    buf note;
    bool is_shell, sniffed;
    unsigned char head[SNIFF_BYTES];
    size_t head_n;
    bool exited, exit_ok;
    int exit_code;
    const char *exit_kind;
    capture output; /* stdout and stderr, merged in write order */
    buf status;
    double launched_at, ended_at;

    json_buf result;
    bool is_error;
} call;

static call *find_job(const calls *cs, const char *call_id);

static bool settled(const call *c)
{
    return c->state == CALL_DONE || c->state == CALL_CANCELLED;
}

static bool is_job(const call *c)
{
    return c->delivers_late || !c->wait;
}

static void free_if_unreferenced(call *c)
{
    if (c->in_turn || c->proc || !settled(c))
        return;
    VEC_REMOVE(c->calls->live, c->calls->nlive, c);
    if (c->output.file_fd >= 0)
        close(c->output.file_fd);
    free(c->output.file_path);
    free(c->output.file_error);
    buf_free(&c->output.kept);
    buf *bufs[] = {&c->input, &c->status, &c->result, &c->note};
    for (size_t i = 0; i < LEN(bufs); i++)
        buf_free(bufs[i]);
    free(c->id);
    free(c->tool_name);
    free(c->program);
    free(c->target);
    free(c->media_type);
    free(c->media_name);
    free(c);
}

static void set_result_text(call *c, span text, bool is_error)
{
    c->result.n = 0;
    content_put_text(&c->result, text);
    c->is_error = is_error;
}

static long long ran_seconds(const call *c)
{
    return c->launched_at ? (long long)(c->ended_at - c->launched_at + 0.5) : 0;
}

/* A call's tool_result. One delivered after its turn, a job's, also names its tool, how it ended
   and how long it ran, for providers that word it as text (providers.md §5). */
static void put_result_block(const call *c, json_list *blocks)
{
    if (!c->delivers_late)
    {
        blocks_add_tool_result(blocks, c->id, c->is_error, buf_span(&c->result));
        return;
    }
    json_printf(
        blocks,
        "%r{'type':'tool_result','tool_use_id':%s,'is_error':%b,'content':%R,'name':%s,'status':%S,"
        "'ran':%D}",
        blocks->n ? "," : "",
        c->id,
        c->is_error,
        buf_span(&c->result),
        c->tool_name,
        buf_span(&c->status),
        ran_seconds(c)
    );
}

static void queue_job_result(call *c)
{
    json_list block = {0};
    json_buf content = {0};
    put_result_block(c, &block);
    json_printf(&content, "[%R]", buf_span(&block));
    inbox_add(c->calls->inbox, FROM_JOB, WHEN_TURN, buf_span(&content));
    buf_free(&block);
    buf_free(&content);
}

/* tool_start, once, when the call is under way: with its process's pid, the file its output goes
   to once it spills or the call becomes a job, and whether its stdin stays open. */
static void announce(call *c)
{
    if (c->announced)
        return;
    c->announced = true;
    json_buf extra = {0};
    if (c->proc)
        json_printf(
            &extra,
            ",'pid':%d,'out':%s,'stdin_open':%b",
            (int)c->proc->pid,
            c->output.file_path,
            c->proc->keep_stdin_open
        );
    output_emit(
        c->calls->output,
        "{'type':'tool_start','run':%d,'turn':%d,'call':%s,'name':%s,'input':%R%r}",
        c->run,
        c->turn,
        c->id,
        c->tool_name,
        buf_span(&c->input),
        buf_cstr(&extra)
    );
    buf_free(&extra);
}

/* The call's result is final: the block a coming user message carries, announced now. */
static void emit_result(call *c)
{
    json_list block = {0};
    announce(c);
    put_result_block(c, &block);
    output_emit(c->calls->output, "{'type':'block','run':%d,'block':%R}", c->run, buf_span(&block));
    buf_free(&block);
}

static void finish_wait(call *waiter, call *job);

/* Ends the job calls waiting for this one, after its job_result is queued, so both arrive
   in the same message. */
static void wake_waiters(call *c)
{
    for (size_t i = 0; i < c->calls->nlive;)
    {
        call *w = c->calls->live[i];
        if (w->target && w->state == CALL_RUNNING && strcmp(w->target, c->id) == 0)
            finish_wait(w, c);
        if (i < c->calls->nlive && c->calls->live[i] == w)
            i++;
    }
}

static void end_call(call *c, const char *end_status)
{
    c->state = CALL_DONE;
    c->ended_at = get_monotonic_seconds();
    if (!c->status.n)
        buf_set(&c->status, S(end_status));
    emit_result(c);
    if (c->delivers_late)
        queue_job_result(c);
    wake_waiters(c);
    free_if_unreferenced(c);
}

static void fail_call(call *c, const char *end_status, span text)
{
    buf_set(&c->status, S(end_status));
    set_result_text(c, text, true);
    end_call(c, end_status);
}

static void cancel_call(call *c, const char *end_status, const char *text)
{
    if (c->proc)
    {
        c->killed = true;
        proc_term(c->proc);
    }
    set_result_text(c, S(text), true);
    buf_set(&c->status, S(end_status));
    c->state = CALL_CANCELLED;
    c->ended_at = get_monotonic_seconds();
    emit_result(c);
    free_if_unreferenced(c);
}

static void stop_capture_to_file(capture *k)
{
    k->file_error = xstrdup(strerror(errno));
    if (k->file_fd >= 0)
        close(k->file_fd);
    k->file_fd = -1;
}

static void write_to_file(capture *k, span bytes)
{
    if (k->file_fd >= 0 && !write_all(k->file_fd, bytes))
        stop_capture_to_file(k);
}

/* <session>.d/calls/<call>.out, with every character of the call id but letters, digits and '-'
   made '_'. */
static char *output_path(const call *c)
{
    buf path = {0};
    buf_printf(&path, "%s.d/calls/", c->calls->config->session_path);
    for (const char *p = c->id; *p; p++)
        buf_putc(&path, isalnum((unsigned char)*p) || *p == '-' ? *p : '_');
    buf_puts(&path, ".out");
    return path.p;
}

/* True once the output is being written to its file (or writing it failed). */
static bool is_filed(const capture *k)
{
    return k->file_fd >= 0 || k->file_error;
}

static void make_parent_dirs(const char *path)
{
    buf dir = {0};
    for (const char *p = strchr(path + 1, '/'); p; p = strchr(p + 1, '/'))
    {
        dir.n = 0;
        buf_put(&dir, path, (size_t)(p - path));
        mkdir(buf_cstr(&dir), 0755);
    }
    buf_free(&dir);
}

static void capture_to_file(call *c)
{
    capture *k = &c->output;
    if (is_filed(k))
        return;
    if (!k->file_path)
        k->file_path = output_path(c);
    make_parent_dirs(k->file_path);
    k->file_fd = open(k->file_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (k->file_fd < 0)
        stop_capture_to_file(k);
    write_to_file(k, buf_span(&k->kept));
}

static void capture_put(call *c, span bytes)
{
    capture *k = &c->output;
    size_t room = k->limit_bytes > k->kept.n ? k->limit_bytes - k->kept.n : 0;
    if (bytes.n > room)
        capture_to_file(c);
    write_to_file(k, bytes);
    buf_put(&k->kept, bytes.p, bytes.n < room ? bytes.n : room);
    k->total_bytes += bytes.n;
}

/* shell takes output_limit from its input; edit keeps everything. */
static size_t output_limit_bytes(const call *c, json input)
{
    if (!c->is_shell)
        return SIZE_MAX;
    double n = json_get_num(json_get(input, "output_limit"), DEFAULT_OUTPUT_LIMIT_BYTES);
    return n < 0 ? 0 : n > MAX_OUTPUT_LIMIT_BYTES ? (size_t)MAX_OUTPUT_LIMIT_BYTES : (size_t)n;
}

static span text_or_empty(const buf *b)
{
    return b->p ? buf_span(b) : S("");
}

/* Text worth showing: no NUL, and few bytes outside UTF-8 in its first 8 KiB. A character cut
   in two at the end of that window does not count. */
static bool looks_like_text(span bytes)
{
    const unsigned char *p = (const unsigned char *)bytes.p;
    size_t n = bytes.n < BINARY_SNIFF_BYTES ? bytes.n : BINARY_SNIFF_BYTES, invalid = 0;
    for (size_t i = 0; i < n;)
    {
        size_t len = p[i] < 0x80 ? 1 : json_utf8_len(p + i, n - i);
        if (!p[i])
            return false;
        if (!len && n - i < 4 && n < bytes.n)
            break;
        invalid += !len;
        i += len ? len : 1;
    }
    return invalid * 8 <= n;
}

static void add_part(buf *header)
{
    buf_puts(header, header->n ? "; " : "[");
}

/* How the program ended. */
static void put_ending(buf *header, const call *c)
{
    add_part(header);
    if (c->overflowed)
        buf_printf(header, "killed: output over %zu bytes, ", (size_t)MAX_LINE_BYTES);
    else if (c->killed)
        buf_puts(header, "killed, ");
    buf_printf(header, "%s %d", c->exit_kind, c->exit_code);
}

/* Where all of the output is, when the result does not hold all of it. */
static void put_file(buf *header, const capture *k)
{
    if (k->file_error)
        buf_printf(header, ", saving all of it to %s failed: %s", k->file_path, k->file_error);
    else
        buf_printf(header, ", all of it in %s", k->file_path);
}

/* The header, always the first line, so nothing the program prints can pass for it: how the
   program ended, then what of its output is not shown. */
static void put_header(buf *header, const call *c, bool shown)
{
    const capture *k = &c->output;
    put_ending(header, c);
    if (!shown)
    {
        add_part(header);
        buf_printf(
            header,
            "%zu bytes of %s not shown",
            k->total_bytes,
            c->media_type ? c->media_type : "binary output"
        );
        if (is_filed(k))
            put_file(header, k);
    }
    else if (k->total_bytes > k->kept.n)
    {
        add_part(header);
        buf_printf(header, "cut at %zu of %zu bytes", k->kept.n, k->total_bytes);
        put_file(header, k);
    }
    else if (k->file_error)
    {
        add_part(header);
        buf_printf(header, "saving output to %s failed: %s", k->file_path, k->file_error);
    }
    buf_putc(header, ']');
}

static void set_media_result(call *c)
{
    json_list blocks = {0};
    blocks_add_media(&blocks, c->media_type, c->media_name, buf_span(&c->output.kept));
    c->result.n = 0;
    json_printf(&c->result, "[%R]", buf_span(&blocks));
    c->is_error = false;
    buf_free(&blocks);
}

/* shell: the header line, then the output as is. An image or PDF from a failed program is not
   shown (it may be cut short, and a provider refuses a broken one), nor is output that is not
   text. */
static void set_output_result(call *c, bool ok)
{
    buf text = {0};
    span out = buf_span(&c->output.kept);
    bool shown = !(c->media_type && !ok) && looks_like_text(out);
    put_header(&text, c, shown);
    if (shown && out.n)
        buf_putc(&text, '\n'), buf_putspan(&text, out);
    set_result_text(c, buf_span(&text), !ok);
    buf_free(&text);
}

static span trim_newline(span text)
{
    if (text.n && text.p[text.n - 1] == '\n')
        text.n--;
    return text;
}

/* edit: what the program said, as an error when it failed. */
static void set_text_result(call *c, bool ok)
{
    span out = text_or_empty(&c->output.kept);
    if (ok)
    {
        set_result_text(c, out, false);
    }
    else
    {
        buf text = {0};
        span said = trim_newline(out);
        if (said.n)
            buf_putspan(&text, said);
        else
            buf_printf(&text, "%s: %s", c->program, buf_cstr(&c->status));
        set_result_text(c, buf_span(&text), true);
        buf_free(&text);
    }
}

/* The media types every provider takes, known by their first bytes. */
static const char *detect_media_type(const unsigned char *b, size_t n)
{
    if (n >= 8 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0)
        return "image/png";
    if (n >= 3 && memcmp(b, "\xff\xd8\xff", 3) == 0)
        return "image/jpeg";
    if (n >= 6 && (memcmp(b, "GIF87a", 6) == 0 || memcmp(b, "GIF89a", 6) == 0))
        return "image/gif";
    if (n >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0)
        return "image/webp";
    if (n >= 5 && memcmp(b, "%PDF-", 5) == 0)
        return "application/pdf";
    return NULL;
}

/* Once: a shell call whose output starts as an image or PDF keeps all of it. */
static void sniff_output(call *c)
{
    if (c->sniffed || !c->is_shell)
        return;
    c->sniffed = true;
    const char *type = detect_media_type(c->head, c->head_n);
    if (!type)
        return;
    c->media_type = xstrdup(type);
    c->output.limit_bytes = SIZE_MAX;
}

static void collect_exec_result(call *c)
{
    bool ok = c->exit_ok && !c->killed;
    const capture *out = &c->output;
    sniff_output(c);
    if (c->killed)
        buf_set(&c->status, S("killed"));
    if (!c->is_shell)
        set_text_result(c, ok);
    else if (ok && c->media_type && out->kept.n && out->kept.n == out->total_bytes)
        set_media_result(c);
    else
        set_output_result(c, ok);
    end_call(c, c->killed ? "killed" : "done");
}

/* Every call's stderr shares its stdout pipe, so there is one stream. */
static void on_exec_chunk(worker *w, enum stream s, span bytes)
{
    call *c = w->owner;
    (void)s;
    if (c->output.kept.n + bytes.n > MAX_LINE_BYTES)
    {
        c->killed = c->overflowed = true;
        proc_term(w);
        return;
    }
    for (size_t i = 0; !c->sniffed && i < bytes.n && c->head_n < SNIFF_BYTES; i++)
        c->head[c->head_n++] = (unsigned char)bytes.p[i];
    if (c->head_n == SNIFF_BYTES)
        sniff_output(c);
    capture_put(c, bytes);
    if (!c->media_type)
        output_emit(
            c->calls->output,
            "{'type':'tool_progress','run':%d,'call':%s,'data':%S}",
            c->run,
            c->id,
            bytes
        );
}

static void on_exec_exit(worker *w)
{
    call *c = w->owner;
    c->proc = NULL;
    c->exited = true;
    c->exit_ok = proc_succeeded(w);
    c->killed |= w->kill_stage != NOT_KILLED;
    c->exit_kind = WIFSIGNALED(w->wait_status) ? "signal" : "exit";
    c->exit_code =
        WIFSIGNALED(w->wait_status) ? WTERMSIG(w->wait_status) : WEXITSTATUS(w->wait_status);
    c->status.n = 0;
    proc_describe_status(w->wait_status, &c->status);
    c->calls->running_execs--;
    free_if_unreferenced(c);
}

static const worker_ops exec_ops = {on_exec_chunk, NULL, on_exec_exit};

/* The program a call runs and what goes to its stdin. */
typedef struct {
    strv argv;
    buf stdin_text;
} launch;

static void push_arg(launch *l, const char *arg)
{
    strv_push(&l->argv, xstrdup(arg));
}

/* A path that starts with '-' would read as an option. */
static bool push_path(launch *l, json path)
{
    char *p = json_decode_dup(path);
    if (!p)
        return false;
    if (*p == '-')
    {
        buf dotted = {0};
        buf_printf(&dotted, "./%s", p);
        free(p);
        p = buf_cstr(&dotted);
    }
    strv_push(&l->argv, p);
    return true;
}

/* The program is a shell, run by name so its argv[0] is its own (busybox dispatches on it). */
static const char *plan_shell(call *c, json input, launch *l)
{
    char *command = json_decode_dup(json_get(input, "command"));
    if (!command)
        return "command is required";
    push_arg(l, "-c");
    strv_push(&l->argv, command);
    json_decode(json_get(input, "stdin"), &l->stdin_text);
    c->is_shell = true;
    c->media_name = json_decode_dup(json_get(input, "stdout_name"));
    return NULL;
}

static const char *plan_edit(json input, launch *l)
{
    if (!push_path(l, json_get(input, "path")))
        return "path is required";
    strv_push(&l->argv, json_decode_dup(json_get(input, "old")));
    strv_push(&l->argv, json_decode_dup(json_get(input, "new")));
    if (!l->argv.v[l->argv.n - 2] || !l->argv.v[l->argv.n - 1])
        return "old and new are required";
    if (json_is_true(json_get(input, "all")))
        push_arg(l, "--all");
    return NULL;
}

/* Every call runs its tool's program from tools.json, looked up on PATH, so a shim earlier on
   PATH, or another program named there, sees each call. */
static void spawn_tool(call *c, const tool_def *def)
{
    json input = buf_span(&c->input);
    launch l = {0};
    buf refusal = {0};
    const char *why = NULL;
    push_arg(&l, def->program);
    if (strcmp(def->name, "shell") == 0)
        why = plan_shell(c, input, &l);
    else
        why = plan_edit(input, &l);
    if (why)
        buf_puts(&refusal, why);
    strv_push(&l.argv, NULL);
    if (refusal.n)
    {
        fail_call(c, "done", buf_span(&refusal));
    }
    else
    {
        c->program = xstrdup(def->program);
        c->output.limit_bytes = output_limit_bytes(c, input);
        spawn_req r = {
            .argv = &l.argv,
            .stdin_bytes = buf_span(&l.stdin_text),
            .keep_stdin_open = json_is(json_get(input, "eof"), 'f'),
            .merge_stderr = true,
            .ops = &exec_ops,
            .owner = c
        };
        c->proc = proc_spawn(c->calls->procs, &r);
        c->launched_at = get_monotonic_seconds();
        c->calls->running_execs++;
        c->output.file_path = output_path(c);
        if (!c->wait)
            capture_to_file(c);
        c->state = CALL_RUNNING;
        announce(c);
    }
    strv_free(&l.argv);
    buf_free(&l.stdin_text);
    buf_free(&refusal);
}

static void start_tool(call *c, const tool_def *def)
{
    int limit = c->calls->config->max_processes;
    if (c->calls->running_execs < limit)
    {
        spawn_tool(c, def);
        return;
    }
    buf text = {0};
    buf_printf(&text, "Not run: already at the --max-processes limit (%d).", limit);
    fail_call(c, "not run", buf_span(&text));
    buf_free(&text);
}

static const struct {
    const char *name;
    int number;
} signal_names[] = {
    {"HUP", SIGHUP},
    {"INT", SIGINT},
    {"QUIT", SIGQUIT},
    {"KILL", SIGKILL},
    {"USR1", SIGUSR1},
    {"USR2", SIGUSR2},
    {"TERM", SIGTERM},
    {"STOP", SIGSTOP},
    {"CONT", SIGCONT},
};

/* A signal by name (TERM or SIGTERM) or number; 0 when absent, -1 when unknown. */
static int parse_signal(json value, buf *text)
{
    if (!value.p)
        return 0;
    if (!json_decode(value, text))
        buf_putspan(text, value);
    const char *name = buf_cstr(text);
    if (strncmp(name, "SIG", 3) == 0)
        name += 3;
    for (size_t i = 0; i < LEN(signal_names); i++)
        if (strcmp(name, signal_names[i].name) == 0)
            return signal_names[i].number;
    char *end;
    long n = strtol(name, &end, 10);
    return *name && !*end && n > 0 && n < 64 ? (int)n : -1;
}

static void put_signal_name(buf *text, int sig)
{
    for (size_t i = 0; i < LEN(signal_names); i++)
        if (signal_names[i].number == sig)
        {
            buf_printf(text, "SIG%s", signal_names[i].name);
            return;
        }
    buf_printf(text, "signal %d", sig);
}

static void add_action(buf *note, const char *action)
{
    buf_printf(note, "%s%s", note->n ? ", " : "", action);
}

/* The end of a job call's wait: how the job ended (its job_result carries its output), or
   that it still runs. */
static void finish_wait(call *waiter, call *job)
{
    buf text = {0}, outcome = {0};
    if (job)
        buf_printf(&outcome, "ended: %s", buf_cstr(&job->status));
    else
        buf_printf(&outcome, "still running after %g s", waiter->deadline - waiter->launched_at);
    if (waiter->note.n)
        buf_printf(
            &text,
            "%s: %s; %s.",
            waiter->target,
            buf_cstr(&waiter->note),
            buf_cstr(&outcome)
        );
    else
        buf_printf(&text, "%s %s.", waiter->target, buf_cstr(&outcome));
    set_result_text(waiter, buf_span(&text), false);
    buf_set(&waiter->status, S("done"));
    end_call(waiter, "done");
    buf_free(&text);
    buf_free(&outcome);
}

/* job {id, stdin?, eof?, signal?, wait?}: in order, input, end of input, a signal to the job's
   process group, then a wait for it to end. Runs in the kernel; it starts no program. */
static void run_job(call *c)
{
    json input = buf_span(&c->input);
    char *id = json_decode_dup(json_get(input, "id"));
    call *job = id ? find_job(c->calls, id) : NULL;
    json stdin_value = json_get(input, "stdin");
    bool eof = json_is_true(json_get(input, "eof"));
    double wait_seconds = json_get_num(json_get(input, "wait"), 0);
    buf stdin_text = {0}, signal_text = {0}, refusal = {0};
    int sig = parse_signal(json_get(input, "signal"), &signal_text);
    json_decode(stdin_value, &stdin_text);
    c->wait = true;
    if (!id)
        buf_puts(&refusal, "id is required");
    else if (!job || !job->proc)
        json_printf(&refusal, "%s is not a running job.", id);
    else if (sig < 0)
        buf_printf(&refusal, "Unknown signal: %s", buf_cstr(&signal_text));
    else if ((stdin_value.p || eof) && !proc_write_stdin(job->proc, buf_span(&stdin_text), eof))
        buf_printf(&refusal, "The stdin of %s is closed.", job->id);
    if (refusal.n)
    {
        fail_call(c, "done", buf_span(&refusal));
    }
    else
    {
        buf action = {0};
        if (stdin_value.p)
            buf_printf(&action, "wrote %zu bytes to its stdin", stdin_text.n),
                add_action(&c->note, buf_cstr(&action));
        if (eof)
            add_action(&c->note, "closed its stdin");
        if (sig)
        {
            proc_signal(job->proc, sig);
            action.n = 0;
            buf_puts(&action, "sent it ");
            put_signal_name(&action, sig);
            add_action(&c->note, buf_cstr(&action));
        }
        buf_free(&action);
        c->target = xstrdup(job->id);
        c->launched_at = get_monotonic_seconds();
        c->deadline = c->launched_at + (wait_seconds > 0 ? wait_seconds : 0);
        c->state = CALL_RUNNING;
        announce(c);
        if (wait_seconds <= 0)
        {
            buf text = {0};
            if (c->note.n)
                buf_printf(&text, "%s: %s.", job->id, buf_cstr(&c->note));
            else
                buf_printf(&text, "%s is running.", job->id);
            set_result_text(c, buf_span(&text), false);
            buf_set(&c->status, S("done"));
            buf_free(&text);
            end_call(c, "done");
        }
    }
    free(id);
    buf_free(&stdin_text);
    buf_free(&signal_text);
    buf_free(&refusal);
}

static void dispatch(call *c)
{
    const tool_def *def = tools_find(c->calls->tools, c->tool_name);
    if (def && strcmp(def->name, "job") == 0)
    {
        run_job(c);
        return;
    }
    if (def)
    {
        start_tool(c, def);
        return;
    }
    buf text = {0};
    buf_printf(&text, "Unknown tool: %s. ", c->tool_name);
    if (c->calls->tools->ndefs)
    {
        buf_puts(&text, "The tools are ");
        tools_put_names(c->calls->tools, &text);
        buf_putc(&text, '.');
    }
    else
    {
        buf_puts(&text, "There are no tools.");
    }
    fail_call(c, "done", buf_span(&text));
    buf_free(&text);
}

static void fail_non_object_input(call *c)
{
    json input = buf_span(&c->input);
    buf raw = {0}, text = {0};
    if (!json_decode(input, &raw))
        buf_set(&raw, input);
    size_t shown = raw.n < MAX_SHOWN_INPUT_BYTES ? raw.n : MAX_SHOWN_INPUT_BYTES;
    while (shown < raw.n && ((unsigned char)raw.p[shown] & 0xC0) == 0x80)
        shown--;
    buf_puts(&text, "input is not a JSON object: ");
    buf_put(&text, raw.p, shown);
    if (shown < raw.n)
        buf_puts(&text, "…");
    fail_call(c, "done", buf_span(&text));
    buf_free(&raw);
    buf_free(&text);
}

static bool advance_call(call *c)
{
    if (c->state != CALL_RUNNING)
        return false;
    bool ran_out = c->deadline && get_monotonic_seconds() >= c->deadline;
    if (c->target && ran_out)
    {
        finish_wait(c, NULL);
        return true;
    }
    if (c->wait && ran_out && !c->target)
    {
        c->wait = false; /* no longer waited for: answered pending, a job */
        return true;
    }
    if (!c->exited)
        return false;
    collect_exec_result(c);
    return true;
}

bool calls_advance(calls *cs)
{
    bool any = false;
    for (size_t i = 0; i < cs->nlive;)
    {
        call *c = cs->live[i];
        any |= advance_call(c);
        if (i < cs->nlive && cs->live[i] == c)
            i++;
    }
    return any;
}

static void start_call(calls *cs, json tool_use, int run, int turn)
{
    call *c = xcalloc(1, sizeof *c);
    c->calls = cs;
    json input = json_get(tool_use, "input");
    c->id = json_decode_dup(json_get(tool_use, "id"));
    c->tool_name = json_decode_dup(json_get(tool_use, "name"));
    if (!c->id)
        c->id = xstrdup("");
    if (!c->tool_name)
        c->tool_name = xstrdup("");
    buf_set(&c->input, input.n ? input : S("{}"));
    c->run = run;
    c->turn = turn;
    c->output.file_fd = -1;
    json wait = json_get(buf_span(&c->input), "wait");
    double wait_seconds = json_is(wait, 'f') ? 0 : json_get_num(wait, -1);
    c->in_turn = true;
    c->wait = !wait.p || json_is(wait, 't') || wait_seconds != 0;
    if (wait_seconds > 0)
        c->deadline = get_monotonic_seconds() + wait_seconds;
    VEC_PUSH(cs->live, cs->nlive, cs->caplive, c);
    VEC_PUSH(cs->turn, cs->nturn, cs->capturn, c);
    if (json_is(buf_span(&c->input), '{'))
        dispatch(c);
    else
        fail_non_object_input(c);
}

double calls_get_next_deadline(const calls *cs)
{
    double next = 0;
    for (size_t i = 0; i < cs->nlive; i++)
    {
        const call *c = cs->live[i];
        if (c->state == CALL_RUNNING && (c->target || c->wait))
            next = earliest_deadline(next, c->deadline);
    }
    return next;
}

json calls_get_tool_schemas(calls *cs)
{
    return tools_get_schemas(cs->tools);
}

size_t calls_start(calls *cs, json assistant_message, int run, int turn)
{
    jiter it = json_iter(json_get(assistant_message, "content"));
    for (json block; json_next(&it, NULL, &block);)
        if (json_eq(json_get(block, "type"), "tool_use"))
            start_call(cs, block, run, turn);
    return cs->nturn;
}

bool calls_is_turn_settled(const calls *cs)
{
    for (size_t i = 0; i < cs->nturn; i++)
        if (cs->turn[i]->wait && !settled(cs->turn[i]))
            return false;
    return true;
}

void calls_abort_turn(calls *cs)
{
    for (size_t i = 0; i < cs->nturn; i++)
    {
        call *c = cs->turn[i];
        if (!settled(c) && c->wait)
            cancel_call(c, "killed", "Killed: the run was aborted.");
    }
}

void calls_take_turn_results(calls *cs, json_list *blocks)
{
    /* A program that exited in the round its wait ran out has its result already. */
    for (size_t i = 0; i < cs->nturn; i++)
        if (cs->turn[i]->state == CALL_RUNNING && cs->turn[i]->exited)
            collect_exec_result(cs->turn[i]);
    for (size_t i = 0; i < cs->nturn; i++)
    {
        call *c = cs->turn[i];
        if (settled(c))
            put_result_block(c, blocks);
        else if ((c->delivers_late = true) && c->proc)
            capture_to_file(c);
    }
    for (size_t i = 0; i < cs->nturn; i++)
    {
        cs->turn[i]->in_turn = false;
        free_if_unreferenced(cs->turn[i]);
    }
    cs->nturn = 0;
}

static call *find_job(const calls *cs, const char *call_id)
{
    for (size_t i = 0; call_id && i < cs->nlive; i++)
        if (is_job(cs->live[i]) && strcmp(call_id, cs->live[i]->id) == 0)
            return cs->live[i];
    return NULL;
}

void calls_kill_jobs(calls *cs)
{
    for (size_t i = 0; i < cs->nlive; i++)
        if (is_job(cs->live[i]) && cs->live[i]->proc)
        {
            cs->live[i]->killed = true;
            proc_term(cs->live[i]->proc);
        }
}

size_t calls_count_running_jobs(const calls *cs)
{
    size_t n = 0;
    for (size_t i = 0; i < cs->nlive; i++)
        n += is_job(cs->live[i]) && !settled(cs->live[i]);
    return n;
}

void calls_put_turn_json(const calls *cs, json_buf *turn)
{
    buf_putc(turn, '[');
    for (size_t i = 0; i < cs->nturn; i++)
        json_printf(
            turn,
            "%r{'call':%s,'name':%s,'state':%s}",
            i ? "," : "",
            cs->turn[i]->id,
            cs->turn[i]->tool_name,
            state_names[cs->turn[i]->state]
        );
    buf_putc(turn, ']');
}

void calls_put_jobs_json(const calls *cs, json_buf *jobs)
{
    bool first = true;
    buf_putc(jobs, '[');
    for (size_t i = 0; i < cs->nlive; i++)
    {
        call *c = cs->live[i];
        if (!is_job(c))
            continue;
        json_printf(
            jobs,
            "%r{'call':%s,'name':%s,'input':%R,'pid':%d,'state':%s}",
            first ? "" : ",",
            c->id,
            c->tool_name,
            buf_span(&c->input),
            c->proc ? (int)c->proc->pid : 0,
            state_names[c->state]
        );
        first = false;
    }
    buf_putc(jobs, ']');
}

void calls_close(calls *cs)
{
    for (size_t i = 0; i < cs->nlive;)
    {
        call *c = cs->live[i];
        if (c->delivers_late && !settled(c))
        {
            if (c->exited)
                collect_exec_result(c);
            else
                fail_call(c, "not run", S("Not run: the kernel stopped."));
        }
        if (i < cs->nlive && cs->live[i] == c)
            i++;
    }
}
