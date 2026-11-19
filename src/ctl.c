#include "ctl.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "calls.h"
#include "driver.h"
#include "inbox.h"
#include "loop.h"
#include "msg.h"
#include "session.h"

typedef struct deferred_entry {
    char     *type, *parent_id;
    json_list members;
    json_buf  ref;
} deferred_entry;

typedef void (*command_fn)(ctl *c, json command, json ref);

static const size_t MAX_STDOUT_BACKLOG_BYTES = 8u << 20;

static void flush_stdout(sink *out)
{
    while (out->pending.n && !out->dead) {
        ssize_t n = write(1, out->pending.p, out->pending.n);
        if (n > 0) buf_drop_front(&out->pending, (size_t)n);
        else if (n < 0 && errno == EAGAIN && !out->blocking) return;
        else if (n < 0 && errno == EINTR) continue;
        else out->dead = true, out->pending.n = 0;
    }
}

static void put_stdout(sink *out, span text)
{
    buf_putspan(&out->pending, text);
    if (out->blocking) flush_stdout(out);
}

void ctl_print(sink *out, span text)
{
    put_stdout(out, text);
    put_stdout(out, S("\n"));
}

void event_raw(sink *out, json line)
{
    if (out->answers_only) return;
    put_stdout(out, json_trim(line));
    put_stdout(out, S("\n"));
}

void event(sink *out, const char *fmt, ...)
{
    json_buf line = { 0 };
    va_list ap;
    va_start(ap, fmt);
    json_vprintf(&line, fmt, ap);
    va_end(ap);
    event_raw(out, buf_span(&line));
    buf_free(&line);
}

void event_notice(sink *out, const char *source, span message)
{
    event(out, "{'type':'notice','source':%s,'message':%S}", source, json_trim(message));
}

void event_error(sink *out, const char *where, span message)
{
    event(out, "{'type':'error','where':%s,'message':%S}", where, message);
}

static void emit_reply(ctl *c, json ref, json object)
{
    json_buf line = { 0 };
    object = json_trim(object);
    buf_put(&line, object.p, object.n - 1);
    if (ref.p) json_printf(&line, ",'ref':%R", ref);
    buf_puts(&line, "}");
    event_raw(&c->out, buf_span(&line));
    buf_free(&line);
}

static void reply_ok(ctl *c, json ref, const char *entry_id)
{
    json_buf reply = { 0 };
    if (!ref.p) return;
    if (entry_id) json_printf(&reply, "{'type':'ok','id':%s}", entry_id);
    else json_printf(&reply, "{'type':'ok'}");
    emit_reply(c, ref, buf_span(&reply));
    buf_free(&reply);
}

static void reply_error(ctl *c, json ref, const char *message)
{
    json_buf reply = { 0 };
    json_printf(&reply, "{'type':'error','where':'command','message':%s}", message);
    emit_reply(c, ref, buf_span(&reply));
    buf_free(&reply);
}

static int choice_index(json v, const char *default_choice, const char *other_choice)
{
    return !v.p || json_eq(v, default_choice) ? 0 : json_eq(v, other_choice) ? 1 : -1;
}

static void enqueue(ctl *c, json command, json ref, enum when when)
{
    json content = json_get(command, "content");
    int source = choice_index(json_get(command, "source"), "prompt", "inject");
    if (!content_valid(content)) reply_error(c, ref, "content must be a string or an array of content blocks");
    else if (source < 0) reply_error(c, ref, "source must be prompt or inject");
    else {
        reply_ok(c, ref, NULL);
        inbox_add(c->inbox, source ? FROM_INJECT : FROM_PROMPT, when, content);
    }
}

static void cmd_send(ctl *c, json command, json ref)
{
    json field = json_get(command, "when");
    enum when when = WHEN_TURN;
    if (field.p && !when_parse(field, &when)) reply_error(c, ref, "when must be now, turn, idle or deferred");
    else enqueue(c, command, ref, when);
}

static void cmd_prompt(ctl *c, json command, json ref) { enqueue(c, command, ref, WHEN_IDLE); }
static void cmd_steer(ctl *c, json command, json ref)  { enqueue(c, command, ref, WHEN_TURN); }

static void cmd_abort(ctl *c, json command, json ref)
{
    driver_abort(c->driver, json_true(json_get(command, "jobs")) ? KILL_JOBS : KEEP_JOBS);
    reply_ok(c, ref, NULL);
}

static void cmd_kill(ctl *c, json command, json ref)
{
    if (calls_kill_job(c->calls, json_get(command, "job"))) reply_ok(c, ref, NULL);
    else reply_error(c, ref, "no such job");
}

static void replace_with_text(char **owned, json string)
{
    char *text = json_text_dup(string);
    if (!text) return;
    free(*owned);
    *owned = text;
}

static void cmd_set(ctl *c, json command, json ref)
{
    driver *d = c->driver;
    json params = json_get(command, "params");
    replace_with_text(&d->provider_model, json_get(command, "model"));
    replace_with_text(&d->system_path, json_get(command, "system"));
    setenv("AKERN_MODEL", d->provider_model, 1);
    if (json_is(params, '{')) {
        json object = json_trim(params);
        buf_set(&d->param_members, json_trim((span){ object.p + 1, object.n - 2 }));
    }
    reply_ok(c, ref, NULL);
}

static void put_entry_members(json entry, json_list *members)
{
    jiter it = json_iter(entry);
    for (json k, v; json_next(&it, &k, &v);)
        if (!json_eq(k, "type") && !json_eq(k, "id") && !json_eq(k, "parent") && !json_eq(k, "ts"))
            json_printf(members, "%r%R:%R", members->n ? "," : "", k, v);
}

static void cmd_append(ctl *c, json command, json ref)
{
    json entry = json_get(command, "entry"), parent = json_get(entry, "parent");
    char *type = json_text_dup(json_get(entry, "type"));
    char *parent_id = parent.p ? (json_null_or_absent(parent) ? xstrdup(AS_ROOT) : json_text_dup(parent)) : NULL;
    if (!type || (parent.p && (!parent_id || (*parent_id && !session_has_entry(c->session, parent_id))))) {
        reply_error(c, ref, "entry needs a type, and parent must be null or an existing id");
        free(type);
        free(parent_id);
        return;
    }
    json_list members = { 0 };
    put_entry_members(entry, &members);
    if (parent_id) {
        deferred_entry df = { .type = type, .parent_id = parent_id, .members = members };
        buf_putspan(&df.ref, ref);
        VEC_PUSH(c->deferred, c->ndeferred, c->capdeferred, df);
        return;
    }
    reply_ok(c, ref, session_append(c->session, type, AT_HEAD, &members));
    buf_free(&members);
    free(type);
}

void ctl_append_deferred_entries(ctl *c)
{
    for (size_t i = 0; i < c->ndeferred; i++) {
        deferred_entry *df = &c->deferred[i];
        const char *id = session_append(c->session, df->type, df->parent_id, &df->members);
        reply_ok(c, df->ref.n ? buf_span(&df->ref) : NOSPAN, id);
        free(df->type);
        free(df->parent_id);
        buf_free(&df->members);
        buf_free(&df->ref);
    }
    c->ndeferred = 0;
}

static void cmd_status(ctl *c, json command, json ref)
{
    json_buf status = { 0 };
    (void)command;
    driver_status(c->driver, &status);
    emit_reply(c, ref, buf_span(&status));
    buf_free(&status);
}

static const struct { const char *type; command_fn run; } commands[] = {
    { "send", cmd_send },     { "prompt", cmd_prompt }, { "steer", cmd_steer },
    { "abort", cmd_abort },   { "kill", cmd_kill },     { "set", cmd_set },
    { "append", cmd_append }, { "status", cmd_status },
};

static void run_command_line(ctl *c, span line)
{
    line = json_trim(line);
    if (!line.n) return;
    if (line.p[0] != '{') {
        json_buf content = { 0 };
        json_put_string(&content, line);
        inbox_add(c->inbox, FROM_PROMPT, WHEN_TURN, buf_span(&content));
        buf_free(&content);
        return;
    }
    if (!json_valid(line)) {
        reply_error(c, NOSPAN, "invalid JSON");
        return;
    }
    json command = line, type = json_get(command, "type"), ref = json_get(command, "ref");
    for (size_t i = 0; i < LEN(commands); i++)
        if (json_eq(type, commands[i].type)) {
            commands[i].run(c, command, ref);
            return;
        }
    reply_error(c, ref, "unknown command");
}

static void take_line(ctl *c, span line)
{
    if (c->skip_rest_of_line) c->skip_rest_of_line = false;
    else if (line.n > c->config->max_line_bytes) reply_error(c, NOSPAN, "line longer than --max-line-bytes");
    else run_command_line(c, line);
}

static void split_lines(ctl *c)
{
    buf *pending = &c->stdin_pending;
    char *nl;
    while (pending->n && (nl = memchr(pending->p, '\n', pending->n))) {
        size_t n = (size_t)(nl - pending->p);
        take_line(c, (span){ pending->p, n });
        buf_drop_front(pending, n + 1);
    }
    if (c->stdin_eof && pending->n) {
        take_line(c, buf_span(pending));
        pending->n = 0;
    }
    if (pending->n > c->config->max_line_bytes) {
        take_line(c, buf_span(pending));
        pending->n = 0;
        c->skip_rest_of_line = true;
    }
}

static void on_stdin(void *ctx, int fd, short revents)
{
    ctl *c = ctx;
    char chunk[65536];
    ssize_t n = read(fd, chunk, sizeof chunk);
    (void)revents;
    if (n > 0) buf_put(&c->stdin_pending, chunk, (size_t)n);
    else if (n == 0 || errno != EAGAIN) c->stdin_eof = true;
    split_lines(c);
}

static void on_stdout(void *ctx, int fd, short revents)
{
    sink *out = ctx;
    (void)fd;
    if (revents & (POLLERR | POLLHUP)) out->dead = true, out->pending.n = 0;
    else flush_stdout(out);
}

void ctl_watch_output(ctl *c, poll_set *set)
{
    if (c->out.pending.n && !c->out.blocking && !c->out.dead) watch_once(set, 1, POLLOUT, on_stdout, &c->out);
}

void ctl_watch(ctl *c, poll_set *set)
{
    ctl_watch_output(c, set);
    if (!c->stdin_eof) watch_once(set, 0, POLLIN, on_stdin, c);
}

void ctl_open(ctl *c)
{
    c->out.answers_only = c->config->print_answers;
    c->out.blocking = isatty(1);
    if (!c->out.blocking) fcntl(1, F_SETFL, fcntl(1, F_GETFL) | O_NONBLOCK);
}

bool ctl_backlogged(const ctl *c) { return c->out.pending.n > MAX_STDOUT_BACKLOG_BYTES; }
bool ctl_eof(const ctl *c)        { return c->stdin_eof; }

void ctl_flush(ctl *c)
{
    fcntl(1, F_SETFL, fcntl(1, F_GETFL) & ~O_NONBLOCK);
    c->out.blocking = true;
    flush_stdout(&c->out);
}
