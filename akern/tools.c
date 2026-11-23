/* tools: the actor that runs tool calls (tools.md §1), from the tool_use blocks of the reply
   that asks for them to their answers. Owns the tool definitions in tools.json
   ($AKERN_HOME/tools.json, else $AKERN_HOME/system/tools.json): each tool's program, and the
   definitions sent to the provider, each without its program; running each shell and edit call
   as its tool's program, at once or refused past --max-processes, its result what the program
   writes on fd 3, else what it said; the job tool (input, end of input, a signal, a wait); each
   call's answer, a block line as it ends, and a released line when its wait runs out (or it has
   none), so the results go out without it; a call still running when the turn's results go out
   becomes a job, answered later; killing calls and jobs on an abort line. Processes are
   process.c's; when the results go out is the inbox's.
   job = a call still running when its turn's results went out, or started with wait: false,
   named by its call id. */

#include "akern.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- tools.json ---- */

static const tool_def *tools_find(const tools *ts, const char *name);

/* Drops the whitespace between tokens, so the request stays one line. */
static void put_compact(json_buf *out, json value)
{
    bool in_string = false, escaped = false;
    for (size_t i = 0; i < value.n; i++)
    {
        char ch = value.p[i];
        if (in_string)
        {
            in_string = escaped || ch != '"';
            escaped = !escaped && ch == '\\';
        }
        else if (ch == '"')
        {
            in_string = true;
        }
        else if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')
        {
            continue;
        }
        buf_putc(out, ch);
    }
}

/* The tools the kernel knows how to call (calls.c); tools.json picks among them. job runs in
   the kernel, the others as their program. */
static const char *const known_tools[] = {"shell", "job", "edit"};

static bool is_known(const char *name)
{
    for (size_t i = 0; i < LEN(known_tools); i++)
        if (strcmp(name, known_tools[i]) == 0)
            return true;
    return false;
}

static void add_tool(tools *ts, const char *path, size_t index, json tool)
{
    char *name = json_decode_dup(json_get(tool, "name"));
    char *program = json_decode_dup(json_get(tool, "program"));
    bool needs_program = name && strcmp(name, "job") != 0;
    const char *problem = !json_is(tool, '{')         ? "is not an object"
                          : !name                     ? "has no name"
                          : !is_known(name)           ? "is not shell, job or edit"
                          : !program && needs_program ? "has no program"
                          : tools_find(ts, name)      ? "is named twice"
                                                      : NULL;
    if (problem)
        die("%s: tool %zu %s", path, index + 1, problem);
    tool_def def = {.name = name, .program = program};
    VEC_PUSH(ts->defs, ts->ndefs, ts->capdefs, def);

    json_put_comma_unless_first(&ts->schemas);
    buf_putc(&ts->schemas, '{');
    bool first = true;
    jiter it = json_iter(tool);
    for (json key, value; json_next(&it, &key, &value);)
    {
        if (json_eq(key, "program"))
            continue;
        if (!first)
            buf_putc(&ts->schemas, ',');
        first = false;
        buf_putspan(&ts->schemas, key);
        buf_putc(&ts->schemas, ':');
        put_compact(&ts->schemas, value);
    }
    buf_putc(&ts->schemas, '}');
}

void tools_open(tools *ts)
{
    const char *home_dir = ts->config->home_dir;
    static const char *const layers[] = {
        "",
        "/system"
    }; /* the user's overlay, then what make install owns */
    buf text = {0}, path = {0};
    bool found = false;
    for (size_t i = 0; i < LEN(layers) && !found; i++)
    {
        path.n = 0;
        buf_printf(&path, "%s%s/tools.json", home_dir, layers[i]);
        found = read_file(path.p, &text);
        if (!found && errno != ENOENT)
            die("%s: %s", path.p, strerror(errno));
    }
    if (!found)
        die("no tools.json in %s or %s/system", home_dir, home_dir);
    json all = json_trim(buf_span(&text));
    if (!json_is_valid(all) || !json_is(all, '['))
        die("%s: not a JSON array", path.p);
    buf_putc(&ts->schemas, '[');
    jiter it = json_iter(all);
    size_t index = 0;
    for (json tool; json_next(&it, NULL, &tool); index++)
        add_tool(ts, path.p, index, tool);
    buf_putc(&ts->schemas, ']');
    buf_free(&text);
    buf_free(&path);
}

static const tool_def *tools_find(const tools *ts, const char *name)
{
    for (size_t i = 0; i < ts->ndefs; i++)
        if (strcmp(ts->defs[i].name, name) == 0)
            return &ts->defs[i];
    return NULL;
}

static void tools_put_names(const tools *ts, buf *text)
{
    for (size_t i = 0; i < ts->ndefs; i++)
    {
        const char *sep = i == 0 ? "" : i + 1 == ts->ndefs ? " and " : ", ";
        buf_printf(text, "%s%s", sep, ts->defs[i].name);
    }
}

/* ---- calls ---- */

enum { KEPT_BYTES = 100000, SHOWN_INPUT_BYTES = 200 };

typedef enum { RUNNING, DONE, CANCELLED } call_state;

static const char *const state_names[] = {"running", "done", "cancelled"};

typedef struct call {
    tools *tools;
    const tool_def *def;
    char *id, *name, *out, *target; /* out: its output file; target: the job a job call acts on */
    json_buf input, kept, result, status;
    buf note; /* what a job call did, for its reply */
    int run, turn;
    call_state state;
    bool in_turn, wait, late, killed, announced;
    process *proc;
    double started, deadline;
} call;

static call *find(const tools *ts, const char *id, bool jobs_only)
{
    for (size_t i = 0; id && i < ts->nlive; i++)
    {
        call *c = ts->live[i];
        if (!strcmp(c->id, id) && (!jobs_only || c->late || !c->wait))
            return c;
    }
    return NULL;
}

static void free_if_done(call *c)
{
    if (c->in_turn || c->proc || c->state == RUNNING)
        return;
    VEC_REMOVE(c->tools->live, c->tools->nlive, c);
    buf *bufs[] = {&c->input, &c->kept, &c->result, &c->status, &c->note};
    for (size_t i = 0; i < LEN(bufs); i++)
        buf_free(bufs[i]);
    free(c->id), free(c->name), free(c->out), free(c->target), free(c);
}

/* The call no longer holds the turn's results back. */
static void release(call *c)
{
    c->wait = false;
    bus_tellf(c->tools->bus, "{'type':'released','call':%s}", c->id);
}

static void set_deadline(call *c, double at)
{
    c->deadline = at;
    bus_tell_at(c->tools->bus, at, "{'type':'due','call':%s}", c->id);
}

/* tool_start, once: with a process, its pid, its output file and whether its stdin stays open. */
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
            c->out,
            c->proc->keep_stdin_open
        );
    bus_printf(
        c->tools->bus,
        "{'type':'tool_start','run':%d,'turn':%d,'call':%s,'name':%s,'input':%R%r}",
        c->run,
        c->turn,
        c->id,
        c->name,
        buf_span(&c->input),
        buf_cstr(&extra)
    );
    buf_free(&extra);
}

static void finish_wait(call *waiter, call *job);

/* The call's answer, final: a block line, which the inbox puts in the call's slot. A job's also
   names its tool, how it ended and how long it ran (providers.md §5). Job calls waiting for this
   one end after it, so both answers go in the same message. */
static void answer(call *c, call_state state, const char *status, json content, bool is_error)
{
    c->state = state;
    buf_set(&c->status, S(status));
    announce(c);
    json_buf block = {0};
    json_printf(
        &block,
        "{'type':'tool_result','tool_use_id':%s,'is_error':%b,'content':%R",
        c->id,
        is_error,
        content
    );
    if (c->late)
        json_printf(
            &block,
            ",'name':%s,'status':%s,'ran':%D",
            c->name,
            status,
            c->started ? (long long)(get_monotonic_seconds() - c->started + 0.5) : 0
        );
    bus_printf(c->tools->bus, "{'type':'block','run':%d,'block':%R}}", c->run, buf_span(&block));
    buf_free(&block);
    for (size_t i = 0; state == DONE && i < c->tools->nlive; i++)
    {
        call *w = c->tools->live[i];
        if (w->target && w->state == RUNNING && !strcmp(w->target, c->id))
            finish_wait(w, c), i = (size_t)-1; /* the list changed: from the top */
    }
    free_if_done(c);
}

static void answer_text(call *c, call_state state, const char *status, span text, bool is_error)
{
    json_buf content = {0};
    json_printf(&content, "[{'type':'text','text':%S}]", text.n ? text : S("(empty)"));
    answer(c, state, status, buf_span(&content), is_error);
    buf_free(&content);
}

static void fail(call *c, const char *status, const char *fmt, ...)
{
    char text[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    answer_text(c, DONE, status, S(text), true);
}

/* ---- programs ---- */

/* A program's stdout and stderr are what it said, streamed as tool_progress; fd 3 is its result,
   a content array (tools.md §1). Without one, the result is what it said. */
static void on_output(process *p, enum stream s, span bytes)
{
    call *c = p->owner;
    buf *into = s == ERR ? &c->result : &c->kept;
    size_t cap = s == ERR ? MAX_LINE_BYTES : KEPT_BYTES, room = into->n < cap ? cap - into->n : 0;
    buf_put(into, bytes.p, bytes.n < room ? bytes.n : room);
    if (s == OUT)
        bus_printf(
            c->tools->bus,
            "{'type':'tool_progress','run':%d,'call':%s,'data':%S}",
            c->run,
            c->id,
            bytes
        );
}

static void on_exit(process *p)
{
    call *c = p->owner;
    c->proc = NULL;
    c->tools->running--;
    if (c->state != RUNNING)
    {
        free_if_done(c);
        return;
    }
    buf status = {0}, text = {0};
    bool killed = c->killed || p->kill_stage != NOT_KILLED, ok = proc_succeeded(p) && !killed;
    proc_describe_status(p->wait_status, &status);
    const char *ended = killed ? "killed" : buf_cstr(&status);
    json fd3 = json_trim(buf_span(&c->result));
    if (json_is(fd3, '[') && json_is_valid(fd3))
    {
        answer(c, DONE, ended, fd3, !ok);
        buf_free(&status);
        return;
    }
    span said = buf_span(&c->kept);
    while (!ok && said.n && said.p[said.n - 1] == '\n')
        said.n--;
    if (!ok && !said.n)
        buf_printf(&text, "%s: %s", c->def->program, buf_cstr(&status));
    else
        buf_putspan(&text, said);
    answer_text(c, DONE, ended, buf_span(&text), !ok);
    buf_free(&status), buf_free(&text);
}

static const process_ops tool_ops = {on_output, NULL, on_exit};

/* A string field as an argument; a path that starts with '-' gets ./ so it is not an option. */
static bool push(strv *argv, json in, const char *field, const char *option, bool is_path)
{
    json value = json_get(in, field);
    char *text = json_decode_dup(value);
    if (option && value.p)
    {
        char number[32];
        snprintf(number, sizeof number, "%.0f", json_get_num(value, 0));
        strv_push(argv, xstrdup(option)), strv_push(argv, xstrdup(text ? text : number));
    }
    else if (text && is_path && text[0] == '-')
    {
        buf dotted = {0};
        buf_printf(&dotted, "./%s", text);
        strv_push(argv, buf_cstr(&dotted));
    }
    else if (text)
        strv_push(argv, xstrdup(text));
    free(text);
    return text != NULL;
}

/* The program's arguments: shell's are -c COMMAND then its options, edit's PATH OLD NEW. */
static const char *plan(call *c, strv *argv)
{
    json in = buf_span(&c->input);
    strv_push(argv, xstrdup(c->def->program));
    if (!strcmp(c->def->name, "shell"))
    {
        strv_push(argv, xstrdup("-c"));
        if (!push(argv, in, "command", NULL, false))
            return "command is required";
        push(argv, in, "output_limit", "--output-limit", false);
        push(argv, in, "stdout_name", "--stdout-name", false);
        return NULL;
    }
    if (!push(argv, in, "path", NULL, true))
        return "path is required";
    if (!push(argv, in, "old", NULL, false) || !push(argv, in, "new", NULL, false))
        return "old and new are required";
    if (json_is_true(json_get(in, "all")))
        strv_push(argv, xstrdup("--all"));
    return NULL;
}

/* Every call runs its tool's program from tools.json, found on PATH; its output file is
   <session>.d/calls/<call>.out, every character of the id but letters, digits and - made _. */
static void run_program(call *c)
{
    tools *ts = c->tools;
    strv argv = {0};
    const char *why = plan(c, &argv);
    if (why)
        fail(c, "done", "%s", why);
    else if (ts->running >= ts->config->max_processes)
        fail(
            c,
            "not run",
            "Not run: already at the --max-processes limit (%d).",
            ts->config->max_processes
        );
    else
    {
        buf stdin_text = {0}, out = {0};
        json_decode(json_get(buf_span(&c->input), "stdin"), &stdin_text);
        buf_printf(&out, "AKERN_OUT=%s.d/calls/", ts->config->session_path);
        for (const char *p = c->id; *p; p++)
            buf_putc(&out, isalnum((unsigned char)*p) || *p == '-' ? *p : '_');
        buf_puts(&out, ".out");
        strv_push(&argv, NULL);
        spawn_req r = {
            .argv = &argv,
            .stdin_bytes = buf_span(&stdin_text),
            .keep_stdin_open = json_is(json_get(buf_span(&c->input), "eof"), 'f'),
            .tool = true,
            .env = buf_cstr(&out),
            .ops = &tool_ops,
            .owner = c
        };
        c->proc = proc_spawn(ts->processes, &r);
        c->out = xstrdup(out.p + strlen("AKERN_OUT="));
        c->started = get_monotonic_seconds();
        ts->running++;
        announce(c);
        buf_free(&stdin_text), buf_free(&out);
    }
    strv_free(&argv);
}

/* ---- the job tool ---- */

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
    const char *name = buf_cstr(text) + (strncmp(buf_cstr(text), "SIG", 3) ? 0 : 3);
    for (size_t i = 0; i < LEN(signal_names); i++)
        if (!strcmp(name, signal_names[i].name))
            return signal_names[i].number;
    char *end;
    long n = strtol(name, &end, 10);
    return *name && !*end && n > 0 && n < 64 ? (int)n : -1;
}

/* A job call's reply: what it did, then how the job ended or that it still runs. */
static void finish_wait(call *waiter, call *job)
{
    buf text = {0};
    buf_puts(&text, waiter->target);
    if (waiter->note.n)
        buf_printf(&text, ": %s;", buf_cstr(&waiter->note));
    if (job)
        buf_printf(&text, " ended: %s.", buf_cstr(&job->status));
    else if (waiter->deadline)
        buf_printf(&text, " still running after %g s.", waiter->deadline - waiter->started);
    else if (waiter->note.n)
        text.p[text.n - 1] = '.';
    else
        buf_puts(&text, " is running.");
    answer_text(waiter, DONE, "done", buf_span(&text), false);
    buf_free(&text);
}

/* job {id, stdin?, eof?, signal?, wait?}: input, end of input, a signal to the job's process
   group, then a wait for it to end. It runs in the kernel and starts no program. */
static void run_job(call *c)
{
    json in = buf_span(&c->input), stdin_value = json_get(in, "stdin");
    char *id = json_decode_dup(json_get(in, "id"));
    call *job = find(c->tools, id, true);
    bool eof = json_is_true(json_get(in, "eof"));
    double wait = json_get_num(json_get(in, "wait"), 0);
    buf stdin_text = {0}, signal_text = {0}, *note = &c->note;
    int sig = parse_signal(json_get(in, "signal"), &signal_text);
    json_decode(stdin_value, &stdin_text);
    c->wait = true;
    if (!id)
        fail(c, "done", "id is required");
    else if (!job || !job->proc)
        fail(c, "done", "\"%s\" is not a running job.", id);
    else if (sig < 0)
        fail(c, "done", "Unknown signal: %s", buf_cstr(&signal_text));
    else if ((stdin_value.p || eof) && !proc_write_stdin(job->proc, buf_span(&stdin_text), eof))
        fail(c, "done", "The stdin of %s is closed.", job->id);
    else
    {
        if (stdin_value.p)
            buf_printf(note, "wrote %zu bytes to its stdin", stdin_text.n);
        if (eof)
            buf_printf(note, "%sclosed its stdin", note->n ? ", " : "");
        if (sig)
            buf_printf(note, "%ssent it ", note->n ? ", " : ""), proc_signal(job->proc, sig);
        for (size_t i = 0; sig && i < LEN(signal_names); i++)
            if (signal_names[i].number == sig)
                buf_printf(note, "SIG%s", signal_names[i].name), sig = 0;
        if (sig)
            buf_printf(note, "signal %d", sig);
        c->target = xstrdup(job->id);
        c->started = get_monotonic_seconds();
        announce(c);
        if (wait > 0)
            set_deadline(c, c->started + wait);
        else
            finish_wait(c, NULL);
    }
    free(id);
    buf_free(&stdin_text), buf_free(&signal_text);
}

/* ---- calls ---- */

static void start_call(tools *ts, json tool_use, int run, int turn)
{
    call *c = xcalloc(1, sizeof *c);
    json input = json_get(tool_use, "input"), wait = json_get(input, "wait");
    double wait_seconds = json_is(wait, 'f') ? 0 : json_get_num(wait, -1);
    *c = (call){.tools = ts, .run = run, .turn = turn, .in_turn = true};
    c->id = json_decode_dup(json_get(tool_use, "id"));
    c->name = json_decode_dup(json_get(tool_use, "name"));
    c->id = c->id ? c->id : xstrdup("");
    c->name = c->name ? c->name : xstrdup("");
    c->def = tools_find(ts, c->name);
    buf_set(&c->input, input.n ? input : S("{}"));
    c->wait = !wait.p || json_is(wait, 't') || wait_seconds != 0;
    VEC_PUSH(ts->live, ts->nlive, ts->caplive, c);
    VEC_PUSH(ts->turn, ts->nturn, ts->capturn, c);
    if (!c->wait)
        release(c);
    if (!json_is(buf_span(&c->input), '{'))
    {
        buf raw = {0};
        if (!json_decode(buf_span(&c->input), &raw))
            buf_set(&raw, buf_span(&c->input));
        size_t shown = raw.n < SHOWN_INPUT_BYTES ? raw.n : SHOWN_INPUT_BYTES;
        while (shown < raw.n && ((unsigned char)raw.p[shown] & 0xC0) == 0x80)
            shown--;
        fail(
            c,
            "done",
            "input is not a JSON object: %.*s%s",
            (int)shown,
            raw.p,
            shown < raw.n ? "…" : ""
        );
        buf_free(&raw);
    }
    else if (c->def && !c->def->program)
        run_job(c);
    else if (c->def)
    {
        if (wait_seconds > 0)
            set_deadline(c, get_monotonic_seconds() + wait_seconds);
        run_program(c);
    }
    else
    {
        buf names = {0};
        tools_put_names(ts, &names);
        fail(
            c,
            "done",
            "Unknown tool: %s. %s%s%s",
            c->name,
            names.n ? "The tools are " : "There are no tools.",
            buf_cstr(&names),
            names.n ? "." : ""
        );
        buf_free(&names);
    }
}

/* A deadline: a job call's wait ends; a shell call no longer holds the results back. */
static void on_due(call *c)
{
    if (!c || c->state != RUNNING || !c->deadline || get_monotonic_seconds() < c->deadline)
        return;
    if (c->target)
        finish_wait(c, NULL);
    else if (c->wait)
        release(c);
}

/* The turn's results went out: calls still running are jobs from now on, answered when they
   end. */
static void close_turn(tools *ts)
{
    for (size_t i = 0; i < ts->nturn; i++)
        ts->turn[i]->late |= ts->turn[i]->state == RUNNING;
    for (size_t i = 0; i < ts->nturn; i++)
        ts->turn[i]->in_turn = false, free_if_done(ts->turn[i]);
    ts->nturn = 0;
}

/* Kills the turn's unfinished waited calls (wait: false ones run on) and, with jobs, every job. */
static void abort_calls(tools *ts, bool jobs)
{
    for (size_t i = 0; i < ts->nturn; i++)
    {
        call *c = ts->turn[i];
        if (c->state != RUNNING || !c->wait)
            continue;
        if (c->proc)
            c->killed = true, proc_term(c->proc);
        answer_text(c, CANCELLED, "killed", S("Killed: the run was aborted."), true);
    }
    for (size_t i = 0; jobs && i < ts->nlive; i++)
        if ((ts->live[i]->late || !ts->live[i]->wait) && ts->live[i]->proc)
            ts->live[i]->killed = true, proc_term(ts->live[i]->proc);
}

void tools_on_line(void *ctx, json line)
{
    tools *ts = ctx;
    json type = json_get(line, "type"), meta = json_get(line, "meta");
    bool reply = json_eq(json_get(json_get(line, "message"), "role"), "assistant");
    if (json_eq(type, "turn_start"))
        free(ts->reply_id), ts->reply_id = json_decode_dup(json_get(line, "id"));
    else if (json_eq(type, "abort"))
        abort_calls(ts, json_is_true(json_get(line, "jobs")));
    else if (json_eq(type, "due"))
    {
        char *id = json_decode_dup(json_get(line, "call"));
        on_due(find(ts, id, false));
        free(id);
    }
    else if ((json_eq(type, "message") && !reply) || json_eq(type, "pending"))
        close_turn(ts);
    else if (
        json_eq(type, "message") && ts->reply_id && json_eq(json_get(line, "id"), ts->reply_id) &&
        json_eq(json_get(meta, "stop"), "tool_use")
    )
    {
        int run = (int)json_get_num(json_get(meta, "run"), 0),
            turn = (int)json_get_num(json_get(meta, "turn"), 0);
        jiter it = json_iter(json_get(json_get(line, "message"), "content"));
        for (json block; json_next(&it, NULL, &block);)
            if (json_eq(json_get(block, "type"), "tool_use"))
                start_call(ts, block, run, turn);
    }
}

void tools_put_status(void *ctx, json_list *members)
{
    tools *ts = ctx;
    json_printf(members, "%r'calls':[", members->n ? "," : "");
    for (size_t i = 0; i < ts->nturn; i++)
        json_printf(
            members,
            "%r{'call':%s,'name':%s,'state':%s}",
            i ? "," : "",
            ts->turn[i]->id,
            ts->turn[i]->name,
            state_names[ts->turn[i]->state]
        );
    json_printf(members, "],'jobs':[");
    bool first = true;
    for (size_t i = 0; i < ts->nlive; i++)
    {
        call *c = ts->live[i];
        if (!c->late && c->wait)
            continue;
        json_printf(
            members,
            "%r{'call':%s,'name':%s,'input':%R,'pid':%d,'state':%s}",
            first ? "" : ",",
            c->id,
            c->name,
            buf_span(&c->input),
            c->proc ? (int)c->proc->pid : 0,
            state_names[c->state]
        );
        first = false;
    }
    buf_putc(members, ']');
}
