#include "calls.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "files.h"
#include "inbox.h"
#include "msg.h"
#include "proc.h"
#include "sink.h"

static const char TOOL_SCHEMAS[] =
    "[{'name':'exec',"
    "'description':'Run a program. argv[0] is looked up on PATH and run directly, with no shell; "
    "for pipes, redirects or scripts, run a shell yourself, e.g. argv [\\'sh\\', \\'-c\\', \\'...\\']. stdin is "
    "empty unless you pass text in stdin, which needs no quoting or escaping. The result is a JSON "
    "object with stdout, stderr and exit, or signal instead of exit if the program was killed. "
    "You get the result when the program finishes, unless a message arrives first: then it keeps "
    "running and its result arrives later as a message. Set wait to false for a program you do not "
    "need to wait for (a server, a watcher, a sub-agent you will steer). If the program writes an image or PDF "
    "to stdout, set stdout_type to its MIME type to receive it as an image or document instead of text. stdout is cut at "
    "stdout_limit bytes and stderr at stderr_limit bytes (default 100000 each); when one is longer, "
    "all of it is saved to a file and the result adds stdout_total and stdout_file (or "
    "stderr_total and stderr_file), so you can read the rest with tail, sed or grep, or rerun "
    "with a larger limit. To talk to a program while it runs (a REPL, a sub-agent), set "
    "stdin_open: its stdin stays open after any stdin text, and you send it more with write and the job named in the reply.',"
    "'input_schema':{'type':'object','properties':{"
    "'argv':{'type':'array','items':{'type':'string'},'minItems':1},"
    "'stdin':{'type':'string'},"
    "'stdin_open':{'type':'boolean'},"
    "'wait':{'type':'boolean'},"
    "'stdout_type':{'type':'string'},"
    "'stdout_limit':{'type':'integer','minimum':0},"
    "'stderr_limit':{'type':'integer','minimum':0}},"
    "'required':['argv']}},"
    "{'name':'read',"
    "'description':'Read a file. Text comes back exactly: 2000 lines from offset (a 1-based line number, "
    "default 1), or limit lines, at most 100000 bytes; a longer file ends with a note saying how to go on. "
    "For an image, PDF or other media file, pass its MIME type in type to receive it as an image or document.',"
    "'input_schema':{'type':'object','properties':{"
    "'path':{'type':'string'},"
    "'offset':{'type':'integer','minimum':1},"
    "'limit':{'type':'integer','minimum':1},"
    "'type':{'type':'string'}},"
    "'required':['path']}},"
    "{'name':'write',"
    "'description':'Write text to a file, or to the stdin of a running program started with stdin_open. "
    "A file is replaced atomically, or added to with append; its directory must already exist. "
    "For a job, content goes to its stdin; set close to end its input after it.',"
    "'input_schema':{'type':'object','properties':{"
    "'path':{'type':'string'},"
    "'job':{'type':'string'},"
    "'content':{'type':'string'},"
    "'append':{'type':'boolean'},"
    "'close':{'type':'boolean'}},"
    "'required':['content']}}]";

enum { DEFAULT_OUTPUT_LIMIT_BYTES = 100000, MAX_SHOWN_INPUT_BYTES = 200 };
static const double MAX_OUTPUT_LIMIT_BYTES = 1e12;

static const char *const stream_names[NSTREAM] = { "stdout", "stderr" };
static const char *const limit_keys[NSTREAM] = { "stdout_limit", "stderr_limit" };
static const char *const stream_exts[NSTREAM] = { "out", "err" };

typedef enum { CALL_WAITING, CALL_RUNNING, CALL_DONE, CALL_CANCELLED } call_state;

static const char *const state_names[] = { "waiting", "running", "done", "cancelled" };

typedef struct {
    buf     kept;
    size_t  total_bytes, limit_bytes;
    int     file_fd;
    char   *file_path, *file_error;
} capture;

typedef struct call {
    calls      *calls;
    char       *id, *tool_name;
    json_buf    input;
    int         run, turn;
    call_state  state;
    bool        in_turn, wait, delivers_late, killed;

    worker     *proc;
    bool        exited, exit_ok;
    int         exit_code;
    const char *exit_kind;
    capture     output[NSTREAM];
    buf         status;
    double      launched_at, ended_at;

    json_buf    result;
    bool        is_error;
} call;

static call *find_job(const calls *cs, json call_id);

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
    if (c->in_turn || c->proc || !settled(c)) return;
    VEC_REMOVE(c->calls->live, c->calls->nlive, c);
    for (int s = 0; s < NSTREAM; s++) {
        if (c->output[s].file_fd >= 0) close(c->output[s].file_fd);
        free(c->output[s].file_path);
        free(c->output[s].file_error);
        buf_free(&c->output[s].kept);
    }
    buf *bufs[] = { &c->input, &c->status, &c->result };
    for (size_t i = 0; i < LEN(bufs); i++) buf_free(bufs[i]);
    free(c->id);
    free(c->tool_name);
    free(c);
}

static void set_result_text(call *c, span text, bool is_error)
{
    c->result.n = 0;
    content_from_text(&c->result, text);
    c->is_error = is_error;
}

static long long ran_seconds(const call *c)
{
    return c->launched_at ? (long long)(c->ended_at - c->launched_at + 0.5) : 0;
}

static void queue_job_result(call *c)
{
    json_buf block = { 0 }, content = { 0 };
    json_printf(&block, "{'type':'job_result','tool_use_id':%s,'name':%s,'is_error':%b,'status':%S,'ran':%D,'content':%R}",
                c->id, c->tool_name, c->is_error, buf_span(&c->status), ran_seconds(c), buf_span(&c->result));
    json_printf(&content, "[%R]", buf_span(&block));
    inbox_add(c->calls->inbox, FROM_JOB, WHEN_TURN, buf_span(&content));
    buf_free(&block);
    buf_free(&content);
}

static void end_call(call *c, const char *end_status)
{
    c->state = CALL_DONE;
    c->ended_at = monotonic_seconds();
    event(c->calls->sink, "{'type':'tool_end','run':%d,'call':%s,'is_error':%b,'status':%s}", c->run, c->id,
          c->is_error, end_status);
    if (c->delivers_late) queue_job_result(c);
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
    if (c->proc) {
        c->killed = true;
        proc_term(c->proc);
    }
    set_result_text(c, S(text), true);
    c->state = CALL_CANCELLED;
    event(c->calls->sink, "{'type':'tool_end','run':%d,'call':%s,'is_error':true,'status':%s}", c->run, c->id,
          end_status);
    free_if_unreferenced(c);
}

static void stop_capture_to_file(capture *k)
{
    k->file_error = xstrdup(strerror(errno));
    if (k->file_fd >= 0) close(k->file_fd);
    k->file_fd = -1;
}

static void write_to_file(capture *k, span bytes)
{
    if (k->file_fd >= 0 && !write_all(k->file_fd, bytes)) stop_capture_to_file(k);
}

static char *output_path(const call *c, enum stream s)
{
    buf path = { 0 };
    buf_printf(&path, "%s.d", c->calls->config->session_path);
    mkdir(path.p, 0755);
    buf_puts(&path, "/calls");
    mkdir(buf_cstr(&path), 0755);
    buf_putc(&path, '/');
    for (const char *p = c->id; *p; p++) buf_putc(&path, isalnum((unsigned char)*p) || *p == '-' ? *p : '_');
    buf_printf(&path, ".%s", stream_exts[s]);
    return path.p;
}

static void capture_to_file(call *c, enum stream s)
{
    capture *k = &c->output[s];
    k->file_path = output_path(c, s);
    k->file_fd = open(k->file_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (k->file_fd < 0) stop_capture_to_file(k);
    write_to_file(k, buf_span(&k->kept));
}

static void capture_put(call *c, enum stream s, span bytes)
{
    capture *k = &c->output[s];
    size_t room = k->limit_bytes > k->kept.n ? k->limit_bytes - k->kept.n : 0;
    if (bytes.n > room && !k->file_path) capture_to_file(c, s);
    write_to_file(k, bytes);
    buf_put(&k->kept, bytes.p, bytes.n < room ? bytes.n : room);
    k->total_bytes += bytes.n;
}

static size_t output_limit_bytes(json input, enum stream s)
{
    if (s == OUT && json_get(input, "stdout_type").p) return SIZE_MAX;
    double n = json_num(json_get(input, limit_keys[s]), DEFAULT_OUTPUT_LIMIT_BYTES);
    return n < 0 ? 0 : n > MAX_OUTPUT_LIMIT_BYTES ? (size_t)MAX_OUTPUT_LIMIT_BYTES : (size_t)n;
}

static void capture_all_to_files(call *c)
{
    for (int s = 0; s < NSTREAM; s++)
        if (!c->output[s].file_path)
            capture_to_file(c, s);
}

static void add_pending_block(call *c, json_list *blocks)
{
    json_printf(blocks, "%r{'type':'tool_pending','tool_use_id':%s,'name':%s,'state':%s",
                blocks->n ? "," : "", c->id, c->tool_name, state_names[c->state]);
    if (c->proc) {
        capture_all_to_files(c);
        json_printf(blocks, ",'pid':%d,'out':%s,'err':%s,'stdin_open':%b",
                    (int)c->proc->pid, c->output[OUT].file_path, c->output[ERR].file_path, c->proc->keep_stdin_open);
    }
    buf_putc(blocks, '}');
}

static span text_or_empty(const buf *b)
{
    return b->p ? buf_span(b) : S("");
}

static void put_exit_members(const call *c, json_buf *out)
{
    json_printf(out, "'%r':%d", c->exit_kind, c->exit_code);
    if (c->killed) json_printf(out, ",'killed':true");
    for (int s = 0; s < NSTREAM; s++) {
        const capture *k = &c->output[s];
        if (k->total_bytes > k->kept.n)
            json_printf(out, ",'%r_total':%D,'%r_file':%s",
                        stream_names[s], (long long)k->total_bytes, stream_names[s], k->file_path);
        if (k->file_error) json_printf(out, ",'%r_file_error':%s", stream_names[s], k->file_error);
    }
}

static void set_media_result(call *c, json media_type)
{
    json_list blocks = { 0 };
    json_buf info = { 0 };
    char *type = json_text_dup(media_type);
    blocks_add_media(&blocks, type, NULL, buf_span(&c->output[OUT].kept));
    free(type);
    json_printf(&info, "{'stderr':%S,", text_or_empty(&c->output[ERR].kept));
    put_exit_members(c, &info);
    buf_putc(&info, '}');
    blocks_add_text(&blocks, buf_span(&info));
    c->result.n = 0;
    json_printf(&c->result, "[%R]", buf_span(&blocks));
    c->is_error = false;
    buf_free(&blocks);
    buf_free(&info);
}

static void set_output_result(call *c, bool ok, json media_type)
{
    json_buf output = { 0 };
    buf omitted = { 0 };
    span out = text_or_empty(&c->output[OUT].kept);
    if (media_type.p && c->output[OUT].total_bytes) {
        buf_printf(&omitted, "(%zu bytes not shown: the program failed)", c->output[OUT].total_bytes);
        out = buf_span(&omitted);
    }
    json_printf(&output, "{'stdout':%S,'stderr':%S,", out, text_or_empty(&c->output[ERR].kept));
    put_exit_members(c, &output);
    buf_putc(&output, '}');
    set_result_text(c, buf_span(&output), !ok);
    buf_free(&output);
    buf_free(&omitted);
}

static void collect_exec_result(call *c)
{
    bool ok = c->exit_ok && !c->killed;
    json media_type = json_get(buf_span(&c->input), "stdout_type");
    if (c->killed) buf_set(&c->status, S("killed"));
    if (ok && media_type.p && c->output[OUT].kept.n) set_media_result(c, media_type);
    else set_output_result(c, ok, media_type);
    end_call(c, c->killed ? "killed" : "done");
}

static void on_exec_chunk(worker *w, enum stream s, span bytes)
{
    call *c = w->owner;
    if (c->output[OUT].kept.n + c->output[ERR].kept.n + bytes.n > c->calls->config->max_line_bytes) {
        c->killed = true;
        proc_term(w);
        return;
    }
    capture_put(c, s, bytes);
    if (s == ERR)
        event(c->calls->sink, "{'type':'tool_progress','run':%d,'call':%s,'data':%S}", c->run, c->id, bytes);
}

static void on_exec_exit(worker *w)
{
    call *c = w->owner;
    c->proc = NULL;
    c->exited = true;
    c->exit_ok = proc_succeeded(w);
    c->exit_kind = WIFSIGNALED(w->wait_status) ? "signal" : "exit";
    c->exit_code = WIFSIGNALED(w->wait_status) ? WTERMSIG(w->wait_status) : WEXITSTATUS(w->wait_status);
    c->status.n = 0;
    proc_describe_status(w->wait_status, &c->status);
    c->calls->running_execs--;
    free_if_unreferenced(c);
}

static const worker_ops exec_ops = { on_exec_chunk, NULL, on_exec_exit };

static bool parse_argv(json input, strv *argv)
{
    jiter it = json_iter(json_get(input, "argv"));
    for (json arg; json_next(&it, NULL, &arg);) {
        if (!json_is(arg, '"')) return false;
        strv_push(argv, json_text_dup(arg));
    }
    strv_push(argv, NULL);
    return argv->n > 1;
}

static void spawn_exec(call *c)
{
    json input = buf_span(&c->input);
    strv argv = { 0 };
    if (!parse_argv(input, &argv)) {
        fail_call(c, "done", S("argv must be a non-empty array of strings"));
        strv_free(&argv);
        return;
    }
    buf stdin_text = { 0 };
    json_text(json_get(input, "stdin"), &stdin_text);
    for (int s = 0; s < NSTREAM; s++) c->output[s].limit_bytes = output_limit_bytes(input, s);
    spawn_req r = { .argv = &argv, .stdin_bytes = buf_span(&stdin_text),
                    .keep_stdin_open = json_true(json_get(input, "stdin_open")),
                    .ops = &exec_ops, .owner = c };
    c->proc = proc_spawn(c->calls->procs, &r);
    c->launched_at = monotonic_seconds();
    strv_free(&argv);
    buf_free(&stdin_text);
    c->calls->running_execs++;
    if (c->delivers_late) capture_all_to_files(c);
    c->state = CALL_RUNNING;
}

static bool tool_write_job(const calls *cs, json input, json_buf *result)
{
    json job_id = json_get(input, "job");
    call *job = find_job(cs, job_id);
    bool close_stdin = json_true(json_get(input, "close"));
    buf stdin_text = { 0 }, reply = { 0 };
    json_text(json_get(input, "content"), &stdin_text);
    bool ok = job && job->proc && proc_write_stdin(job->proc, buf_span(&stdin_text), close_stdin);
    if (ok)
        buf_printf(&reply, "Wrote %zu bytes to the stdin of %s%s.", stdin_text.n, job->id,
                   close_stdin ? ", then closed it" : "");
    else if (job && job->proc) buf_printf(&reply, "The stdin of %s is closed.", job->id);
    else json_printf(&reply, "%R is not a running job.", job_id.p ? job_id : S("\"\""));
    content_from_text(result, buf_span(&reply));
    buf_free(&stdin_text);
    buf_free(&reply);
    return ok;
}

static void run_builtin(call *c)
{
    json input = buf_span(&c->input);
    c->result.n = 0;
    bool ok = strcmp(c->tool_name, "read") == 0 ? tool_read(input, &c->result)
            : json_get(input, "job").p          ? tool_write_job(c->calls, input, &c->result)
                                                : tool_write_file(input, &c->result);
    c->is_error = !ok;
    buf_set(&c->status, S(ok ? "done" : "failed"));
    end_call(c, "done");
}

static void dispatch(call *c)
{
    if (strcmp(c->tool_name, "exec") == 0) {
        c->state = CALL_WAITING;
    } else if (strcmp(c->tool_name, "read") == 0 || strcmp(c->tool_name, "write") == 0) {
        run_builtin(c);
    } else {
        buf text = { 0 };
        buf_printf(&text, "Unknown tool: %s. The tools are exec, read and write.", c->tool_name);
        fail_call(c, "done", buf_span(&text));
        buf_free(&text);
    }
}

static void fail_non_object_input(call *c)
{
    json input = buf_span(&c->input);
    buf raw = { 0 }, text = { 0 };
    if (!json_text(input, &raw)) buf_set(&raw, input);
    size_t shown = raw.n < MAX_SHOWN_INPUT_BYTES ? raw.n : MAX_SHOWN_INPUT_BYTES;
    while (shown < raw.n && ((unsigned char)raw.p[shown] & 0xC0) == 0x80) shown--;
    buf_puts(&text, "input is not a JSON object: ");
    buf_put(&text, raw.p, shown);
    if (shown < raw.n) buf_puts(&text, "…");
    fail_call(c, "done", buf_span(&text));
    buf_free(&raw);
    buf_free(&text);
}

static bool advance_call(call *c)
{
    if (c->state == CALL_WAITING && c->calls->running_execs < c->calls->config->max_processes) spawn_exec(c);
    else if (c->state == CALL_RUNNING && c->exited) collect_exec_result(c);
    else return false;
    return true;
}

bool calls_advance(calls *cs)
{
    bool any = false;
    for (size_t i = 0; i < cs->nlive;) {
        call *c = cs->live[i];
        any |= advance_call(c);
        if (i < cs->nlive && cs->live[i] == c) i++;
    }
    return any;
}

static void start_call(calls *cs, json tool_use, int run, int turn)
{
    call *c = xcalloc(1, sizeof *c);
    c->calls = cs;
    json input = json_get(tool_use, "input");
    c->id = json_text_dup(json_get(tool_use, "id"));
    c->tool_name = json_text_dup(json_get(tool_use, "name"));
    if (!c->id) c->id = xstrdup("");
    if (!c->tool_name) c->tool_name = xstrdup("");
    buf_set(&c->input, input.n ? input : S("{}"));
    c->run = run;
    c->turn = turn;
    c->output[OUT].file_fd = c->output[ERR].file_fd = -1;
    json wait = json_get(buf_span(&c->input), "wait");
    c->in_turn = true;
    c->wait = !(wait.p && json_is(wait, 'f'));
    VEC_PUSH(cs->live, cs->nlive, cs->caplive, c);
    VEC_PUSH(cs->turn, cs->nturn, cs->capturn, c);
    event(cs->sink, "{'type':'tool_start','run':%d,'turn':%d,'call':%s,'name':%s,'input':%R}",
          run, turn, c->id, c->tool_name, buf_span(&c->input));
    if (json_is(buf_span(&c->input), '{')) dispatch(c);
    else fail_non_object_input(c);
}

json calls_tool_schemas(calls *cs)
{
    if (!cs->tool_schemas.n) json_printf(&cs->tool_schemas, TOOL_SCHEMAS);
    return buf_span(&cs->tool_schemas);
}

size_t calls_start(calls *cs, json assistant_message, int run, int turn)
{
    jiter it = json_iter(json_get(assistant_message, "content"));
    for (json block; json_next(&it, NULL, &block);)
        if (json_eq(json_get(block, "type"), "tool_use")) start_call(cs, block, run, turn);
    return cs->nturn;
}

bool calls_turn_settled(const calls *cs)
{
    for (size_t i = 0; i < cs->nturn; i++)
        if (cs->turn[i]->wait ? !settled(cs->turn[i]) : cs->turn[i]->state == CALL_WAITING) return false;
    return true;
}

void calls_abort_turn(calls *cs)
{
    for (size_t i = 0; i < cs->nturn; i++) {
        call *c = cs->turn[i];
        if (settled(c) || !c->wait) continue;
        if (c->state == CALL_WAITING) cancel_call(c, "cancelled", "Not run: the run was aborted.");
        else cancel_call(c, "killed", "Killed: the run was aborted.");
    }
}

void calls_take_turn_results(calls *cs, json_list *blocks)
{
    for (size_t i = 0; i < cs->nturn; i++) {
        call *c = cs->turn[i];
        if (settled(c)) blocks_add_tool_result(blocks, c->id, c->is_error, buf_span(&c->result));
        else c->delivers_late = true, add_pending_block(c, blocks);
    }
    for (size_t i = 0; i < cs->nturn; i++) {
        cs->turn[i]->in_turn = false;
        free_if_unreferenced(cs->turn[i]);
    }
    cs->nturn = 0;
}

static call *find_job(const calls *cs, json call_id)
{
    for (size_t i = 0; i < cs->nlive; i++)
        if (is_job(cs->live[i]) && json_eq(call_id, cs->live[i]->id)) return cs->live[i];
    return NULL;
}

bool calls_kill_job(calls *cs, json call_id)
{
    call *c = find_job(cs, call_id);
    if (!c) return false;
    if (c->state == CALL_WAITING) {
        fail_call(c, "not run", S("Not run: killed before it started."));
    } else if (c->proc) {
        c->killed = true;
        proc_term(c->proc);
    }
    return true;
}

void calls_kill_jobs(calls *cs)
{
    for (size_t i = 0; i < cs->nlive; i++)
        if (is_job(cs->live[i]) && cs->live[i]->proc) {
            cs->live[i]->killed = true;
            proc_term(cs->live[i]->proc);
        }
}

size_t calls_running_jobs(const calls *cs)
{
    size_t n = 0;
    for (size_t i = 0; i < cs->nlive; i++) n += is_job(cs->live[i]) && !settled(cs->live[i]);
    return n;
}

void calls_turn_json(const calls *cs, json_buf *turn)
{
    buf_putc(turn, '[');
    for (size_t i = 0; i < cs->nturn; i++)
        json_printf(turn, "%r{'call':%s,'name':%s,'state':%s}", i ? "," : "",
                    cs->turn[i]->id, cs->turn[i]->tool_name, state_names[cs->turn[i]->state]);
    buf_putc(turn, ']');
}

void calls_jobs_json(const calls *cs, json_buf *jobs)
{
    bool first = true;
    buf_putc(jobs, '[');
    for (size_t i = 0; i < cs->nlive; i++) {
        call *c = cs->live[i];
        if (!is_job(c)) continue;
        json_printf(jobs, "%r{'call':%s,'name':%s,'input':%R,'pid':%d,'state':%s}",
                    first ? "" : ",", c->id, c->tool_name, buf_span(&c->input),
                    c->proc ? (int)c->proc->pid : 0, state_names[c->state]);
        first = false;
    }
    buf_putc(jobs, ']');
}

void calls_close(calls *cs)
{
    for (size_t i = 0; i < cs->nlive;) {
        call *c = cs->live[i];
        if (c->delivers_late && !settled(c)) {
            if (c->exited) collect_exec_result(c);
            else fail_call(c, "not run", S("Not run: the kernel stopped."));
        }
        if (i < cs->nlive && cs->live[i] == c) i++;
    }
}
