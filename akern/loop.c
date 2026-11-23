/* loop: the one poll loop all I/O waits in, and the kernel's lifetime. Each round hands out the
   bus, then waits: on stdin, stdout, every process's pipes, the signal self-pipe, and the earliest
   timer or kill deadline; what is ready puts its lines on the bus. Owns signals (a SIGINT is an
   interrupt line and an abort, a second one within 3 s or SIGTERM/SIGHUP an exit), and, as an
   actor, the exit line: from then on it reads no stdin, stops every process, and returns once
   none is left. Holds no conversation state; when the kernel is done is the inbox's. Also the
   clocks every timeout and timestamp is read from.
   Modules that wait on a descriptor add it to the poll set the loop lends them each round. */

#include "akern.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    EXIT_INTERRUPTED = 130,
    DOUBLE_SIGINT_SECONDS = 3,
    MAX_POLL_MS = 60000,
};

typedef struct handler {
    ready_fn fn;
    void *ctx;
} handler;

static int signal_write_fd = -1;

void lines_feed(lines *l, span bytes, bool eof, void (*fn)(void *ctx, span line), void *ctx)
{
    buf_putspan(&l->pending, bytes);
    size_t start = 0;
    for (char *nl; (nl = memchr(l->pending.p + l->searched, '\n', l->pending.n - l->searched));)
    {
        size_t end = (size_t)(nl - l->pending.p);
        if (!l->skipping)
            fn(ctx, (span){l->pending.p + start, end - start});
        l->skipping = false;
        start = l->searched = end + 1;
    }
    buf_drop_front(&l->pending, start);
    l->searched = l->pending.n; /* no newline before here: never searched again */
    if ((eof && l->pending.n) || l->pending.n > MAX_LINE_BYTES)
    {
        if (!l->skipping)
            fn(ctx, buf_span(&l->pending));
        l->skipping = !eof;
        l->pending.n = l->searched = 0;
    }
}

void watch_once(poll_set *set, int fd, short events, ready_fn fn, void *ctx)
{
    if (set->n == set->cap)
    {
        set->cap = set->cap ? set->cap * 2 : 64;
        set->fds = xrealloc(set->fds, set->cap * sizeof *set->fds);
        set->handlers = xrealloc(set->handlers, set->cap * sizeof *set->handlers);
    }
    set->fds[set->n] = (struct pollfd){.fd = fd, .events = events};
    set->handlers[set->n++] = (handler){fn, ctx};
}

static void exit_kernel(loop *l, int exit_code)
{
    bus_tellf(l->bus, "{'type':'exit','code':%d}", exit_code);
}

static void on_sigint(loop *l)
{
    double now = get_monotonic_seconds();
    if (l->stopping || now - l->last_sigint_at < DOUBLE_SIGINT_SECONDS)
        exit_kernel(l, EXIT_INTERRUPTED);
    else
    {
        bus_printf(l->bus, "{'type':'interrupt','quit_within':%d}", DOUBLE_SIGINT_SECONDS);
        bus_tellf(l->bus, "{'type':'command','command':{'type':'abort','jobs':false}}");
    }
    l->last_sigint_at = now;
}

static void on_signal(void *ctx, int fd, short revents)
{
    loop *l = ctx;
    unsigned char sigs[64];
    ssize_t n = read(fd, sigs, sizeof sigs);
    (void)revents;
    for (ssize_t i = 0; i < n; i++)
    {
        if (sigs[i] == SIGINT)
            on_sigint(l);
        else if (sigs[i] == SIGTERM || sigs[i] == SIGHUP)
            exit_kernel(l, EXIT_INTERRUPTED);
    }
}

static void forward_signal(int sig)
{
    int saved = errno;
    unsigned char b = (unsigned char)sig;
    write(signal_write_fd, &b, 1);
    errno = saved;
}

void loop_open(loop *l)
{
    int signal_pipe[2];
    if (pipe(signal_pipe) < 0)
        die("pipe: %s", strerror(errno));
    for (int i = 0; i < 2; i++)
    {
        fcntl(signal_pipe[i], F_SETFL, fcntl(signal_pipe[i], F_GETFL) | O_NONBLOCK);
        fcntl(signal_pipe[i], F_SETFD, FD_CLOEXEC);
    }
    l->signal_fd = signal_pipe[0];
    signal_write_fd = signal_pipe[1];
    struct sigaction sa = {.sa_handler = forward_signal, .sa_flags = SA_RESTART};
    sigemptyset(&sa.sa_mask);
    int forwarded[] = {SIGCHLD, SIGINT, SIGTERM, SIGHUP};
    for (size_t i = 0; i < LEN(forwarded); i++)
        sigaction(forwarded[i], &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

static int poll_timeout_ms(loop *l)
{
    double deadline = earliest_deadline(proc_next_kill_at(l->processes), bus_next_timer(l->bus));
    if (!deadline)
        return -1;
    double ms = ceil((deadline - get_monotonic_seconds()) * 1000);
    return ms < 0 ? 0 : ms > MAX_POLL_MS ? MAX_POLL_MS : (int)ms;
}

/* One wait: whatever is ready, timers due and processes done put their lines on the bus. */
static void wait_for_io(loop *l)
{
    poll_set *set = &l->watched;
    set->n = 0;
    watch_once(set, l->signal_fd, POLLIN, on_signal, l);
    bus_watch(l->bus, set);
    if (!l->stopping)
        input_watch(l->input, set);
    proc_watch(l->processes, set, bus_is_backlogged(l->bus));

    if (poll(set->fds, (nfds_t)set->n, poll_timeout_ms(l)) < 0 && errno != EINTR)
        die("poll: %s", strerror(errno));
    for (size_t i = 0; i < set->n; i++)
        if (set->fds[i].revents)
            set->handlers[i].fn(set->handlers[i].ctx, set->fds[i].fd, set->fds[i].revents);

    double now = get_monotonic_seconds();
    proc_settle(l->processes, now);
    bus_fire_timers(l->bus, now);
}

int loop_run(loop *l)
{
    for (;;)
    {
        bus_drain(l->bus);
        if (l->stopping && l->processes->n == 0)
            return l->exit_code;
        wait_for_io(l);
    }
}

void loop_on_line(void *ctx, json line)
{
    loop *l = ctx;
    if (!json_eq(json_get(line, "type"), "exit") || l->stopping)
        return;
    l->stopping = true;
    l->exit_code = (int)json_get_num(json_get(line, "code"), 0);
    proc_term_all(l->processes);
}

double get_monotonic_seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

long long get_unix_seconds(void)
{
    return (long long)time(NULL);
}

double earliest_deadline(double a, double b)
{
    if (!a)
        return b;
    if (!b)
        return a;
    return a < b ? a : b;
}
