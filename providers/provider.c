#include "provider.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t terminated;
static bool emitted, stays, trapped;
static json_buf request;

static void on_sigterm(int sig)
{
    (void)sig;
    terminated = 1;
}

/* No SA_RESTART: a SIGTERM while waiting for the next request ends the wait. */
static void trap_sigterm(void)
{
    if (trapped)
        return;
    struct sigaction sa = {0};
    sa.sa_handler = on_sigterm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    trapped = true;
}

/* One line from stdin, without its newline; false at EOF or once SIGTERM has arrived. */
static bool read_line(buf *line)
{
    line->n = 0;
    for (int c;;)
    {
        if (terminated)
            return false;
        errno = 0;
        if ((c = getc_unlocked(stdin)) == EOF)
        {
            if (errno == EINTR)
            {
                clearerr(stdin);
                continue;
            }
            return line->n > 0;
        }
        if (c == '\n')
            return true;
        buf_putc(line, (char)c);
    }
}

bool provider_next_request(json *out)
{
    trap_sigterm();
    if (!read_line(&request))
        return false;
    emitted = false;
    json r = buf_span(&request);
    bool well_formed = json_is_valid(r) && json_is(json_get(r, "model"), '"') &&
                       json_is(json_get(r, "messages"), '[');
    if (!well_formed)
        provider_emit_error("invalid_request", "expected {\"model\",\"messages\",...} on stdin", 0);
    *out = json_trim(r);
    return true;
}

json provider_read_request(void)
{
    json r;
    if (!provider_next_request(&r))
        provider_emit_error("invalid_request", "expected {\"model\",\"messages\",...} on stdin", 0);
    return r;
}

void provider_stay(void)
{
    stays = true;
}

bool provider_was_aborted(void)
{
    return terminated;
}

static void print_and_free_line(json_buf *line)
{
    buf_putc(line, '\n');
    fwrite(line->p, 1, line->n, stdout);
    fflush(stdout);
    buf_free(line);
}

void provider_emit(const char *fmt, ...)
{
    json_buf line = {0};
    va_list ap;
    va_start(ap, fmt);
    json_vprintf(&line, fmt, ap);
    va_end(ap);
    print_and_free_line(&line);
    emitted = true;
}

bool provider_has_emitted(void)
{
    return emitted;
}

void provider_emit_model(const char *id, long long context, long long max_output)
{
    json_buf line = {0};
    json_printf(&line, "{'id':%s", id);
    if (context)
        json_printf(&line, ",'context':%D", context);
    if (max_output)
        json_printf(&line, ",'max_output':%D", max_output);
    provider_emit("%r}", buf_cstr(&line));
    buf_free(&line);
}

void provider_emit_result(const char *stop, json content, provider_usage usage)
{
    provider_emit(
        "{'type':'result','stop':%s,'message':{'role':'assistant','content':%R},"
        "'usage':{'input':%D,'output':%D,'cache_read':%D,'cache_write':%D}%r}",
        stop,
        content,
        usage.input,
        usage.output,
        usage.cache_read,
        usage.cache_write,
        stays ? ",\"stay\":true" : ""
    );
    if (!stays)
        exit(0);
}

void provider_emit_error(const char *kind, const char *message, long status)
{
    provider_emit_error_at(kind, message, status, NO_BLOCK);
}

void provider_emit_error_at(const char *kind, const char *message, long status, block_at at)
{
    json_buf where = {0};
    if (at.message >= 0)
        json_printf(
            &where,
            ",'at':{'message':%D,'block':%D",
            (long long)at.message,
            (long long)at.block
        );
    if (at.message >= 0 && at.inner >= 0)
        json_printf(&where, ",'inner':%D", (long long)at.inner);
    if (at.message >= 0)
        buf_putc(&where, '}');
    provider_emit(
        "{'type':'error','kind':%s,'message':%s,'status':%D%r}",
        kind,
        message,
        (long long)status,
        buf_cstr(&where)
    );
    exit(1);
}

void open_calls_set(open_calls *oc, json assistant_message)
{
    oc->n = 0;
    jiter it = json_iter(json_get(assistant_message, "content"));
    for (json block; json_next(&it, NULL, &block);)
        if (json_eq(json_get(block, "type"), "tool_use"))
            VEC_PUSH(oc->uses, oc->n, oc->cap, block);
}

static bool same_id(json a, json b)
{
    span x = json_get_escaped_body(a), y = json_get_escaped_body(b);
    return json_is(a, '"') && json_is(b, '"') && x.n == y.n && !memcmp(x.p, y.p, x.n);
}

bool ids_contain(const json *ids, size_t n, json tool_use_id)
{
    for (size_t i = 0; i < n; i++)
        if (same_id(ids[i], tool_use_id))
            return true;
    return false;
}

bool open_calls_take(open_calls *oc, json tool_use_id)
{
    for (size_t i = 0; i < oc->n; i++)
        if (same_id(json_get(oc->uses[i], "id"), tool_use_id))
        {
            memmove(oc->uses + i, oc->uses + i + 1, (oc->n - i - 1) * sizeof *oc->uses);
            oc->n--;
            return true;
        }
    return false;
}

/* <session>.d/calls/<call>.out, as the kernel names a call's output file (akern.md §5). */
static void put_output_path(buf *text, json session, json id)
{
    json_decode(session, text);
    buf_puts(text, ".d/calls/");
    size_t from = text->n;
    json_decode(id, text);
    for (size_t i = from; i < text->n; i++)
        if (!isalnum((unsigned char)text->p[i]) && text->p[i] != '-')
            text->p[i] = '_';
    buf_puts(text, ".out");
}

void provider_describe_unanswered(buf *text, json tool_use)
{
    json id = json_get(tool_use, "id"), input = json_get(tool_use, "input");
    json session = json_get(buf_span(&request), "session");
    if (json_eq(json_get(tool_use, "name"), "job"))
    {
        buf_puts(text, "Not finished yet; its result arrives later as a message.");
        return;
    }
    buf_puts(text, "Still running in the background as job \"");
    json_decode(id, text);
    buf_puts(text, "\", in its own process group");
    if (json_is(session, '"'))
    {
        buf_puts(text, "; its output so far is in ");
        put_output_path(text, session, id);
    }
    buf_puts(text, ". Its result arrives later as a message.");
    if (json_is(json_get(input, "eof"), 'f'))
        buf_puts(text, " Its stdin is open: send it input with the job tool.");
}

void provider_describe_pending(buf *text, json pending)
{
    json pid = json_get(pending, "pid"), stdout_path = json_get(pending, "out"),
         stderr_path = json_get(pending, "err");
    if (!json_eq(json_get(pending, "state"), "running"))
    {
        buf_puts(text, "Not finished yet; its result arrives later as a message.");
        return;
    }
    buf_puts(text, "Still running in the background");
    if (json_get_num(pid, 0) > 0)
        buf_printf(text, " (pid %.0f, its own process group)", json_get_num(pid, 0));
    if (json_is(stdout_path, '"') || json_is(stderr_path, '"'))
    {
        buf_puts(text, "; its output so far is in ");
        json_decode(stdout_path, text);
        if (json_is(stdout_path, '"') && json_is(stderr_path, '"'))
            buf_puts(text, " and ");
        json_decode(stderr_path, text);
    }
    buf_puts(text, ". Its result arrives later as a message.");
    if (json_is_true(json_get(pending, "stdin_open")))
    {
        buf_puts(text, " Its stdin is open: send it input with the job tool, id \"");
        json_decode(json_get(pending, "tool_use_id"), text);
        buf_puts(text, "\".");
    }
}

static void put_attribute(buf *text, const char *name, json value)
{
    if (!json_is(value, '"'))
        return;
    buf_printf(text, " %s=\"", name);
    json_decode(value, text);
    buf_putc(text, '"');
}

void provider_describe_job(buf *text, json job_result)
{
    json content = json_get(job_result, "content"), ran_seconds = json_get(job_result, "ran");
    buf_puts(text, "<job");
    put_attribute(text, "call", json_get(job_result, "tool_use_id"));
    put_attribute(text, "name", json_get(job_result, "name"));
    put_attribute(text, "status", json_get(job_result, "status"));
    if (ran_seconds.p)
        buf_printf(text, " ran=\"%.0fs\"", json_get_num(ran_seconds, 0));
    buf_puts(text, ">\n");
    json_decode(content, text);
    jiter it = json_iter(content);
    for (json block; json_next(&it, NULL, &block);)
        if (json_eq(json_get(block, "type"), "text"))
            json_decode(json_get(block, "text"), text);
    buf_puts(text, "\n</job>");
}
