/* input: everything the kernel reads from stdin (akern.md §3). Owns reading it and splitting it
   into lines; replying to a line that is no command (invalid JSON, too long, unknown); telling
   each command, and a plain line as a send, to the bus for the inbox; stdin's end, an eof line;
   and, as an actor, the status reply, built from every actor's members in its place among the
   commands. */

#include "akern.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

static void reply_error(input *c, json ref, const char *message)
{
    if (ref.p)
        bus_printf(
            c->bus,
            "{'type':'error','where':'command','message':%s,'ref':%R}",
            message,
            ref
        );
    else
        bus_printf(c->bus, "{'type':'error','where':'command','message':%s}", message);
}

/* A line of stdin: text is a send; send, abort, append and status are told as commands, which
   the inbox checks and carries out, all but status. */
static void on_stdin_line(void *ctx, span line)
{
    input *c = ctx;
    line = json_trim(line);
    json type = json_get(line, "type");
    if (line.n > MAX_LINE_BYTES)
        reply_error(c, NOSPAN, "line longer than 64 MiB");
    else if (line.n && line.p[0] != '{')
        bus_tellf(c->bus, "{'type':'command','command':{'type':'send','content':%S}}", line);
    else if (line.n && !json_is_valid(line))
        reply_error(c, NOSPAN, "invalid JSON");
    else if (
        json_eq(type, "send") || json_eq(type, "abort") || json_eq(type, "append") ||
        json_eq(type, "status")
    )
        bus_tellf(c->bus, "{'type':'command','command':%R}", line);
    else if (line.n)
        reply_error(c, json_get(line, "ref"), "unknown command");
}

static void on_stdin(void *ctx, int fd, short revents)
{
    input *c = ctx;
    char chunk[65536];
    ssize_t n = read(fd, chunk, sizeof chunk);
    (void)revents;
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
        return;
    c->stdin_eof = n <= 0;
    lines_feed(&c->stdin, (span){chunk, n > 0 ? (size_t)n : 0}, c->stdin_eof, on_stdin_line, c);
    if (c->stdin_eof)
        bus_tellf(c->bus, "{'type':'eof'}");
}

void input_watch(input *c, poll_set *set)
{
    if (!c->stdin_eof)
        watch_once(set, 0, POLLIN, on_stdin, c);
}

/* status, answered in its place among the commands: each actor's members, in wiring order. */
void input_on_line(void *ctx, json line)
{
    input *c = ctx;
    json command = json_get(line, "command");
    if (!json_eq(json_get(line, "type"), "command") ||
        !json_eq(json_get(command, "type"), "status"))
        return;
    json_list members = {0};
    for (size_t i = 0; i < c->nactors; i++)
        if (c->actors[i].put_status)
            c->actors[i].put_status(c->actors[i].ctx, &members);
    json ref = json_get(command, "ref");
    if (ref.p)
        bus_printf(c->bus, "{'type':'status',%r,'ref':%R}", buf_cstr(&members), ref);
    else
        bus_printf(c->bus, "{'type':'status',%r}", buf_cstr(&members));
    buf_free(&members);
}
