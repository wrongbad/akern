#include "provider.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static volatile sig_atomic_t terminated;
static bool emitted;
static json_buf request;

static void on_sigterm(int sig)
{
    (void)sig;
    terminated = 1;
}

static void trap_sigterm(void)
{
    struct sigaction sa = { 0 };
    sa.sa_handler = on_sigterm;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
}

static void read_all(FILE *f, buf *bytes)
{
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_put(bytes, chunk, n);
}

json provider_request(void)
{
    trap_sigterm();
    read_all(stdin, &request);
    json r = buf_span(&request);
    if (!json_valid(r) || !json_is(json_get(r, "model"), '"') || !json_is(json_get(r, "messages"), '['))
        provider_error("invalid_request", "expected {\"model\",\"messages\",...} on stdin", 0);
    return json_trim(r);
}

bool provider_aborted(void)
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
    json_buf line = { 0 };
    va_list ap;
    va_start(ap, fmt);
    json_vprintf(&line, fmt, ap);
    va_end(ap);
    print_and_free_line(&line);
    emitted = true;
}

bool provider_emitted(void)
{
    return emitted;
}

void provider_model(const char *id, long long context, long long max_output)
{
    json_buf line = { 0 };
    json_printf(&line, "{'id':%s", id);
    if (context) json_printf(&line, ",'context':%D", context);
    if (max_output) json_printf(&line, ",'max_output':%D", max_output);
    provider_emit("%r}", buf_cstr(&line));
    buf_free(&line);
}

void provider_result(const char *stop, json content, provider_usage usage)
{
    provider_emit("{'type':'result','stop':%s,'message':{'role':'assistant','content':%R},"
                  "'usage':{'input':%D,'output':%D,'cache_read':%D,'cache_write':%D}}",
                  stop, content, usage.input, usage.output, usage.cache_read, usage.cache_write);
    exit(0);
}

void provider_error(const char *kind, const char *message, long status)
{
    provider_error_at(kind, message, status, NO_BLOCK);
}

void provider_error_at(const char *kind, const char *message, long status, block_at at)
{
    json_buf where = { 0 };
    if (at.message >= 0) json_printf(&where, ",'at':{'message':%D,'block':%D", (long long)at.message, (long long)at.block);
    if (at.message >= 0 && at.inner >= 0) json_printf(&where, ",'inner':%D", (long long)at.inner);
    if (at.message >= 0) buf_putc(&where, '}');
    provider_emit("{'type':'error','kind':%s,'message':%s,'status':%D%r}", kind, message, (long long)status, buf_cstr(&where));
    exit(1);
}

void provider_describe_pending(buf *text, json pending)
{
    json pid = json_get(pending, "pid"), stdout_path = json_get(pending, "out"), stderr_path = json_get(pending, "err");
    if (!json_eq(json_get(pending, "state"), "running")) {
        buf_puts(text, "Not finished yet; its result arrives later as a message.");
        return;
    }
    buf_puts(text, "Still running in the background");
    if (json_num(pid, 0) > 0) buf_printf(text, " (pid %.0f, its own process group)", json_num(pid, 0));
    if (json_is(stdout_path, '"') || json_is(stderr_path, '"')) {
        buf_puts(text, "; its output so far is in ");
        json_text(stdout_path, text);
        if (json_is(stdout_path, '"') && json_is(stderr_path, '"')) buf_puts(text, " and ");
        json_text(stderr_path, text);
    }
    buf_puts(text, ". Its result arrives later as a message.");
    if (json_true(json_get(pending, "stdin_open"))) {
        buf_puts(text, " Its stdin is open: send it input with write and job \"");
        json_text(json_get(pending, "tool_use_id"), text);
        buf_puts(text, "\".");
    }
}

static void put_attribute(buf *text, const char *name, json value)
{
    if (!json_is(value, '"')) return;
    buf_printf(text, " %s=\"", name);
    json_text(value, text);
    buf_putc(text, '"');
}

void provider_describe_job(buf *text, json job_result)
{
    json content = json_get(job_result, "content"), ran_seconds = json_get(job_result, "ran");
    buf_puts(text, "<job");
    put_attribute(text, "call", json_get(job_result, "tool_use_id"));
    put_attribute(text, "name", json_get(job_result, "name"));
    put_attribute(text, "status", json_get(job_result, "status"));
    if (ran_seconds.p) buf_printf(text, " ran=\"%.0fs\"", json_num(ran_seconds, 0));
    buf_puts(text, ">\n");
    json_text(content, text);
    jiter it = json_iter(content);
    for (json block; json_next(&it, NULL, &block);)
        if (json_eq(json_get(block, "type"), "text")) json_text(json_get(block, "text"), text);
    buf_puts(text, "\n</job>");
}
