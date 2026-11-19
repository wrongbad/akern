#include "ctl.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "calls.h"
#include "driver.h"
#include "inbox.h"
#include "msg.h"
#include "session.h"
#include "sink.h"

typedef void (*command_fn)(ctl *c, json command, json ref);

static void emit_reply(ctl *c, json ref, json object)
{
    json_buf line = { 0 };
    object = json_trim(object);
    buf_put(&line, object.p, object.n - 1);
    if (ref.p) json_printf(&line, ",'ref':%R", ref);
    buf_puts(&line, "}");
    event_raw(c->sink, buf_span(&line));
    buf_free(&line);
}

static void reply_ok(ctl *c, json ref)
{
    if (ref.p) event(c->sink, "{'type':'ok','ref':%R}", ref);
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
        reply_ok(c, ref);
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
    reply_ok(c, ref);
}

static void cmd_kill(ctl *c, json command, json ref)
{
    if (calls_kill_job(c->calls, json_get(command, "job"))) reply_ok(c, ref);
    else reply_error(c, ref, "no such job");
}

static void cmd_set(ctl *c, json command, json ref)
{
    if (driver_set(c->driver, json_get(command, "model"), json_get(command, "system"), json_get(command, "params")))
        reply_ok(c, ref);
    else reply_error(c, ref, "system file cannot be read");
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
    char *parent_id = parent.p ? (json_null_or_absent(parent) ? xstrdup(AS_ROOT) : json_text_dup(parent)) : AT_HEAD;
    json_list members = { 0 };
    put_entry_members(entry, &members);
    if (!type || (parent.p && !parent_id) || !driver_append(c->driver, type, parent_id, &members, ref))
        reply_error(c, ref, "entry needs a type, and parent must be null or an existing id");
    buf_free(&members);
    free(type);
    free(parent_id);
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

void ctl_watch(ctl *c, poll_set *set)
{
    if (!c->stdin_eof) watch_once(set, 0, POLLIN, on_stdin, c);
}

bool ctl_eof(const ctl *c) { return c->stdin_eof; }
