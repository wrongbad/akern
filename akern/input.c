#include "input.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "inbox.h"
#include "message.h"
#include "output.h"
#include "session.h"
#include "turns.h"

typedef void (*command_fn)(input *c, json command, json ref);

static void emit_reply(input *c, json ref, json object)
{
    json_buf line = {0};
    object = json_trim(object);
    buf_put(&line, object.p, object.n - 1);
    if (ref.p)
        json_printf(&line, ",'ref':%R", ref);
    buf_puts(&line, "}");
    output_emit_raw(c->output, buf_span(&line));
    buf_free(&line);
}

static void reply_ok(input *c, json ref)
{
    if (ref.p)
        output_emit(c->output, "{'type':'ok','ref':%R}", ref);
}

static void reply_error(input *c, json ref, const char *message)
{
    json_buf reply = {0};
    json_printf(&reply, "{'type':'error','where':'command','message':%s}", message);
    emit_reply(c, ref, buf_span(&reply));
    buf_free(&reply);
}

static int choice_index(json v, const char *default_choice, const char *other_choice)
{
    return !v.p || json_eq(v, default_choice) ? 0 : json_eq(v, other_choice) ? 1 : -1;
}

static void cmd_send(input *c, json command, json ref)
{
    json content = json_get(command, "content"), when_field = json_get(command, "when");
    int source = choice_index(json_get(command, "source"), "prompt", "inject");
    enum when when = WHEN_TURN;
    if (when_field.p && !when_parse(when_field, &when))
        reply_error(c, ref, "when must be now, turn, idle or deferred");
    else if (!content_is_valid(content))
        reply_error(c, ref, "content must be a string or an array of content blocks");
    else if (source < 0)
        reply_error(c, ref, "source must be prompt or inject");
    else
    {
        reply_ok(c, ref);
        inbox_add(c->inbox, source ? FROM_INJECT : FROM_PROMPT, when, content);
    }
}

static void cmd_abort(input *c, json command, json ref)
{
    turns_abort(c->turns, json_is_true(json_get(command, "jobs")) ? KILL_JOBS : KEEP_JOBS);
    reply_ok(c, ref);
}

static void put_entry_members(json entry, json_list *members)
{
    jiter it = json_iter(entry);
    for (json k, v; json_next(&it, &k, &v);)
        if (!json_eq(k, "type") && !json_eq(k, "id") && !json_eq(k, "parent") && !json_eq(k, "ts"))
            json_printf(members, "%r%R:%R", members->n ? "," : "", k, v);
}

static void cmd_append(input *c, json command, json ref)
{
    json entry = json_get(command, "entry"), parent = json_get(entry, "parent");
    char *type = json_decode_dup(json_get(entry, "type"));
    char *parent_id =
        parent.p ? (json_is_null_or_absent(parent) ? xstrdup(AS_ROOT) : json_decode_dup(parent))
                 : AT_HEAD;
    json_list members = {0};
    put_entry_members(entry, &members);
    bool valid = type && (!parent.p || parent_id);
    bool appended = valid && turns_append(c->turns, type, parent_id, &members, ref);
    if (!appended)
        reply_error(c, ref, "entry needs a type, and parent must be null or an existing id");
    buf_free(&members);
    free(type);
    free(parent_id);
}

static void cmd_status(input *c, json command, json ref)
{
    json_buf status = {0};
    (void)command;
    turns_put_status(c->turns, &status);
    emit_reply(c, ref, buf_span(&status));
    buf_free(&status);
}

static const struct {
    const char *type;
    command_fn run;
} commands[] = {
    {"send", cmd_send},
    {"abort", cmd_abort},
    {"append", cmd_append},
    {"status", cmd_status},
};

static void run_command_line(input *c, span line)
{
    line = json_trim(line);
    if (!line.n)
        return;
    if (line.p[0] != '{')
    {
        json_buf content = {0};
        json_put_string(&content, line);
        inbox_add(c->inbox, FROM_PROMPT, WHEN_TURN, buf_span(&content));
        buf_free(&content);
        return;
    }
    if (!json_is_valid(line))
    {
        reply_error(c, NOSPAN, "invalid JSON");
        return;
    }
    json command = line, type = json_get(command, "type"), ref = json_get(command, "ref");
    for (size_t i = 0; i < LEN(commands); i++)
        if (json_eq(type, commands[i].type))
        {
            commands[i].run(c, command, ref);
            return;
        }
    reply_error(c, ref, "unknown command");
}

static void take_line(input *c, span line)
{
    if (c->skip_rest_of_line)
        c->skip_rest_of_line = false;
    else if (line.n > MAX_LINE_BYTES)
        reply_error(c, NOSPAN, "line longer than 64 MiB");
    else
        run_command_line(c, line);
}

static void split_lines(input *c)
{
    buf *pending = &c->stdin_pending;
    char *nl;
    while (pending->n && (nl = memchr(pending->p, '\n', pending->n)))
    {
        size_t n = (size_t)(nl - pending->p);
        take_line(c, (span){pending->p, n});
        buf_drop_front(pending, n + 1);
    }
    if (c->stdin_eof && pending->n)
    {
        take_line(c, buf_span(pending));
        pending->n = 0;
    }
    if (pending->n > MAX_LINE_BYTES)
    {
        take_line(c, buf_span(pending));
        pending->n = 0;
        c->skip_rest_of_line = true;
    }
}

static void on_stdin(void *ctx, int fd, short revents)
{
    input *c = ctx;
    char chunk[65536];
    ssize_t n = read(fd, chunk, sizeof chunk);
    (void)revents;
    if (n > 0)
        buf_put(&c->stdin_pending, chunk, (size_t)n);
    else if (n == 0 || errno != EAGAIN)
        c->stdin_eof = true;
    split_lines(c);
}

void input_watch(input *c, poll_set *set)
{
    if (!c->stdin_eof)
        watch_once(set, 0, POLLIN, on_stdin, c);
}

bool input_has_ended(const input *c)
{
    return c->stdin_eof;
}
