#include "output.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

static const size_t MAX_STDOUT_BACKLOG_BYTES = 8u << 20;

static void flush_stdout(output *out)
{
    while (out->pending.n && !out->dead)
    {
        ssize_t n = write(1, out->pending.p, out->pending.n);
        if (n > 0)
            buf_drop_front(&out->pending, (size_t)n);
        else if (n < 0 && errno == EAGAIN && !out->blocking)
            return;
        else if (n < 0 && errno == EINTR)
            continue;
        else
            out->dead = true, out->pending.n = 0;
    }
}

static void put_line(output *out, span text)
{
    buf_putspan(&out->pending, text);
    buf_putc(&out->pending, '\n');
    if (out->blocking)
        flush_stdout(out);
}

void output_emit_raw(output *out, json line)
{
    if (!out->answers_only)
        put_line(out, json_trim(line));
}

void output_emit(output *out, const char *fmt, ...)
{
    json_buf line = {0};
    va_list ap;
    va_start(ap, fmt);
    json_vprintf(&line, fmt, ap);
    va_end(ap);
    output_emit_raw(out, buf_span(&line));
    buf_free(&line);
}

void output_emit_notice(output *out, const char *source, span message)
{
    output_emit(out, "{'type':'notice','source':%s,'message':%S}", source, json_trim(message));
}

void output_emit_error(output *out, const char *where, span message)
{
    output_emit(out, "{'type':'error','where':%s,'message':%S}", where, message);
}

void output_write_answer(output *out, span answer, json error)
{
    if (!out->answers_only)
        return;
    if (answer.n)
        put_line(out, answer);
    if (!error.p)
        return;
    buf message_text = {0};
    json_decode(json_get(error, "message"), &message_text);
    fprintf(stderr, "akern: %s\n", buf_cstr(&message_text));
    buf_free(&message_text);
}

static void on_stdout(void *ctx, int fd, short revents)
{
    output *out = ctx;
    (void)fd;
    if (revents & (POLLERR | POLLHUP))
        out->dead = true, out->pending.n = 0;
    else
        flush_stdout(out);
}

void output_watch(output *out, poll_set *set)
{
    if (out->pending.n && !out->blocking && !out->dead)
        watch_once(set, 1, POLLOUT, on_stdout, out);
}

void output_open(output *out, bool answers_only)
{
    out->answers_only = answers_only;
    out->blocking = isatty(1);
    if (!out->blocking)
        fcntl(1, F_SETFL, fcntl(1, F_GETFL) | O_NONBLOCK);
}

bool output_is_backlogged(const output *out)
{
    return out->pending.n > MAX_STDOUT_BACKLOG_BYTES;
}

void output_flush(output *out)
{
    fcntl(1, F_SETFL, fcntl(1, F_GETFL) & ~O_NONBLOCK);
    out->blocking = true;
    flush_stdout(out);
}
