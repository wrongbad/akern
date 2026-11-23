/* bus: the one stream of lines the kernel moves on (akern.md §4), and the only way its modules
   talk. Owns the queue lines wait in and handing each one out, in the order put: a said line to
   stdout, to the session file and then to every actor; a told line to the actors only. Putting
   only queues: a line put while lines are handed out waits its turn, so an actor that reacts to
   a line never runs inside another's reaction. Also owns lines told for later (timers); stdout
   itself, its backlog, print mode (-p: only each run's answer, a failed run's error on stderr)
   and the last blocking flush; and the session file (akern.md §5): its owner lock, reading it
   once on resume, appending every said line as said, new ids, and stopping the kernel when it
   can no longer be written. What a line says is the module's that put it. */

#include "akern.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct timer {
    double at;
    buf line;
} timer;

static void lock_owner(bus *b)
{
    buf path = {0};
    buf_printf(&path, "%s.owner", b->config->session_path);
    b->session.owner_lock_fd = open(buf_cstr(&path), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    struct flock lk = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    if (b->session.owner_lock_fd < 0)
        die("%s: %s", path.p, strerror(errno));
    if (fcntl(b->session.owner_lock_fd, F_SETLK, &lk) < 0)
    {
        char owner_json[256] = {0};
        pread(b->session.owner_lock_fd, owner_json, sizeof owner_json - 1, 0);
        die("session %s is owned by another kernel (pid %d)",
            b->config->session_path,
            (int)json_get_num(json_get(S(owner_json), "pid"), 0));
    }
    json_buf owner = {0};
    json_printf(&owner, "{'pid':%d}\n", (int)getpid());
    ftruncate(b->session.owner_lock_fd, 0);
    pwrite(b->session.owner_lock_fd, owner.p, owner.n, 0);
    buf_free(&owner);
    buf_free(&path);
}

/* ---- the session file ---- */

bool bus_open_session(bus *b)
{
    session *s = &b->session;
    s->id_rng = (uint64_t)get_unix_seconds() ^ ((uint64_t)getpid() << 32) ^
                (uint64_t)(get_monotonic_seconds() * 1e9);
    s->fd = open(b->config->session_path, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (s->fd < 0)
        die("%s: %s", b->config->session_path, strerror(errno));
    lock_owner(b);
    struct stat st;
    if (fstat(s->fd, &st) < 0)
        die("%s: %s", b->config->session_path, strerror(errno));
    return st.st_size == 0;
}

void bus_replay(bus *b, void (*fn)(void *ctx, json line), void *ctx)
{
    buf file = {0};
    if (!read_file(b->config->session_path, &file))
        die("%s: %s", b->config->session_path, strerror(errno));
    for (size_t at = 0; at < file.n;)
    {
        const char *nl = memchr(file.p + at, '\n', file.n - at);
        size_t len = nl ? (size_t)(nl - (file.p + at)) : file.n - at;
        if (json_is_valid((span){file.p + at, len}))
            fn(ctx, (span){file.p + at, len});
        at += len + 1;
    }
    b->session.ends_mid_line = file.n && file.p[file.n - 1] != '\n';
    buf_free(&file);
}

/* Appends a said line; after a torn last line, on a line of its own. */
static void write_session(bus *b, json line)
{
    session *s = &b->session;
    if (s->failed)
        return;
    json_buf text = {0};
    if (s->ends_mid_line)
        buf_putc(&text, '\n');
    buf_putspan(&text, line);
    buf_putc(&text, '\n');
    s->failed = !write_all(s->fd, buf_span(&text));
    s->ends_mid_line = false;
    buf_free(&text);
}

void bus_new_id(bus *b, char id[NEW_ID_CHARS + 1])
{
    uint64_t *r = &b->session.id_rng;
    *r ^= *r << 13;
    *r ^= *r >> 7;
    *r ^= *r << 17;
    snprintf(id, NEW_ID_CHARS + 1, "%08x", (unsigned)(*r >> 16));
}

/* ---- stdout ---- */

static const size_t MAX_STDOUT_BACKLOG_BYTES = 8u << 20;

static void flush_stdout(bus *b)
{
    while (b->pending.n && !b->dead)
    {
        ssize_t n = write(1, b->pending.p, b->pending.n);
        if (n > 0)
            buf_drop_front(&b->pending, (size_t)n);
        else if (n < 0 && errno == EAGAIN && !b->blocking)
            return;
        else if (n < 0 && errno == EINTR)
            continue;
        else
            b->dead = true, b->pending.n = 0;
    }
}

static void put_stdout_line(bus *b, span text)
{
    buf_putspan(&b->pending, text);
    buf_putc(&b->pending, '\n');
    if (b->blocking)
        flush_stdout(b);
}

/* Print mode: the text of a run's last assistant message at its end, and why it failed. */
static void print_answer(bus *b, json line)
{
    json type = json_get(line, "type");
    if (json_eq(type, "run_start"))
        b->answer.n = 0;
    else if (json_eq(type, "message"))
    {
        json message = json_get(line, "message");
        if (!json_eq(json_get(message, "role"), "assistant"))
            return;
        json content = json_get(message, "content");
        jiter it = json_iter(content);
        b->answer.n = 0;
        if (json_is(content, '"'))
            json_decode(content, &b->answer);
        for (json block; json_next(&it, NULL, &block);)
            if (json_eq(json_get(block, "type"), "text"))
                json_decode(json_get(block, "text"), &b->answer);
    }
    else if (json_eq(type, "run_end"))
    {
        if (b->answer.n)
            put_stdout_line(b, buf_span(&b->answer));
        json error = json_get(line, "error");
        if (!json_eq(json_get(line, "reason"), "error") || !json_is(error, '{'))
            return;
        buf text = {0};
        json_decode(json_get(error, "message"), &text);
        fprintf(stderr, "akern: %s\n", buf_cstr(&text));
        buf_free(&text);
    }
}

static void hand_out(bus *b, json line, bool said)
{
    if (said && b->config->print_answers)
        print_answer(b, line);
    else if (said)
        put_stdout_line(b, line);
    if (said)
        write_session(b, line);
    if (said && b->session.failed && !b->session.lost)
    {
        b->session.lost = true;
        bus_put_error(b, "kernel", S("session append failed; stopping"));
        bus_tell(b, S("{\"type\":\"exit\",\"code\":1}"));
    }
    for (size_t i = 0; i < b->nactors; i++)
        b->actors[i].on_line(b->actors[i].ctx, line);
}

static void enqueue(bus *b, char kind, json line)
{
    buf_putc(&b->queue, kind);
    buf_putspan(&b->queue, json_trim(line));
    buf_putc(&b->queue, '\n');
}

void bus_put(bus *b, json line)
{
    enqueue(b, 'S', line);
}

void bus_tell(bus *b, json line)
{
    enqueue(b, 'T', line);
}

static void put_formatted(bus *b, char kind, const char *fmt, va_list ap)
{
    json_buf line = {0};
    json_vprintf(&line, fmt, ap);
    enqueue(b, kind, buf_span(&line));
    buf_free(&line);
}

void bus_printf(bus *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    put_formatted(b, 'S', fmt, ap);
    va_end(ap);
}

void bus_tellf(bus *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    put_formatted(b, 'T', fmt, ap);
    va_end(ap);
}

void bus_put_error(bus *b, const char *where, span message)
{
    bus_printf(b, "{'type':'error','where':%s,'message':%S}", where, message);
}

void bus_tell_at(bus *b, double at, const char *fmt, ...)
{
    timer t = {.at = at};
    va_list ap;
    va_start(ap, fmt);
    json_vprintf(&t.line, fmt, ap);
    va_end(ap);
    VEC_PUSH(b->timers, b->ntimers, b->captimers, t);
}

double bus_next_timer(const bus *b)
{
    double next = 0;
    for (size_t i = 0; i < b->ntimers; i++)
        next = earliest_deadline(next, b->timers[i].at);
    return next;
}

void bus_fire_timers(bus *b, double now)
{
    for (size_t i = 0; i < b->ntimers;)
    {
        if (b->timers[i].at > now)
        {
            i++;
            continue;
        }
        bus_tell(b, buf_span(&b->timers[i].line));
        buf_free(&b->timers[i].line);
        b->timers[i] = b->timers[--b->ntimers];
    }
}

void bus_drain(bus *b)
{
    buf line = {0};
    while (b->next < b->queue.n)
    {
        const char *start = b->queue.p + b->next;
        const char *nl = memchr(start, '\n', b->queue.n - b->next);
        size_t n = (size_t)(nl - start);
        buf_set(&line, (span){start + 1, n - 1}); /* a copy: lines put meanwhile move the queue */
        b->next += n + 1;
        if (b->next == b->queue.n)
            b->next = b->queue.n = 0;
        hand_out(b, buf_span(&line), *start == 'S');
    }
    buf_free(&line);
}

static void on_stdout(void *ctx, int fd, short revents)
{
    bus *b = ctx;
    (void)fd;
    if (revents & (POLLERR | POLLHUP))
        b->dead = true, b->pending.n = 0;
    else
        flush_stdout(b);
}

void bus_watch(bus *b, poll_set *set)
{
    if (b->pending.n && !b->blocking && !b->dead)
        watch_once(set, 1, POLLOUT, on_stdout, b);
}

void bus_open(bus *b)
{
    b->blocking = isatty(1);
    if (!b->blocking)
        fcntl(1, F_SETFL, fcntl(1, F_GETFL) | O_NONBLOCK);
}

bool bus_is_backlogged(const bus *b)
{
    return b->pending.n > MAX_STDOUT_BACKLOG_BYTES;
}

void bus_close(bus *b)
{
    bus_drain(b);
    fcntl(1, F_SETFL, fcntl(1, F_GETFL) & ~O_NONBLOCK);
    b->blocking = true;
    flush_stdout(b);
    if (b->session.owner_lock_fd >= 0)
        ftruncate(b->session.owner_lock_fd, 0); /* ownership given up */
}
