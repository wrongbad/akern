#include "loop.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "calls.h"
#include "input.h"
#include "output.h"
#include "proc.h"
#include "session.h"
#include "turns.h"

enum {
    EXIT_INTERRUPTED = 130,
    DOUBLE_SIGINT_SECONDS = 3,
    MAX_POLL_MS = 60000,
};

static int signal_write_fd = -1;

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

void shutdown_kernel(loop *l, int exit_code)
{
    if (l->stopping)
        return;
    l->stopping = true;
    l->exit_code = exit_code;
    turns_stop(l->turns);
    proc_term_all(l->procs);
}

static void on_sigint(loop *l)
{
    double now = get_monotonic_seconds();
    if (l->stopping || now - l->last_sigint_at < DOUBLE_SIGINT_SECONDS)
        shutdown_kernel(l, EXIT_INTERRUPTED);
    else
    {
        output_emit(l->output, "{'type':'interrupt','quit_within':%d}", DOUBLE_SIGINT_SECONDS);
        turns_abort(l->turns, KEEP_JOBS);
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
            shutdown_kernel(l, EXIT_INTERRUPTED);
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
    double deadline =
        earliest_deadline(proc_get_next_kill_at(l->procs), calls_get_next_deadline(l->calls));
    if (!deadline)
        return -1;
    double ms = ceil((deadline - get_monotonic_seconds()) * 1000);
    return ms < 0 ? 0 : ms > MAX_POLL_MS ? MAX_POLL_MS : (int)ms;
}

static void wait_for_io(loop *l)
{
    poll_set *set = &l->watched;
    set->n = 0;
    watch_once(set, l->signal_fd, POLLIN, on_signal, l);
    output_watch(l->output, set);
    if (!l->stopping)
        input_watch(l->input, set);
    proc_watch(l->procs, set, output_is_backlogged(l->output));

    if (poll(set->fds, (nfds_t)set->n, poll_timeout_ms(l)) < 0 && errno != EINTR)
        die("poll: %s", strerror(errno));
    for (size_t i = 0; i < set->n; i++)
        if (set->fds[i].revents)
            set->handlers[i].fn(set->handlers[i].ctx, set->fds[i].fd, set->fds[i].revents);

    proc_reap(l->procs);
    proc_escalate_kills(l->procs, get_monotonic_seconds());
}

static void advance(loop *l)
{
    for (bool moved = true; moved;)
    {
        moved = proc_deliver_exits(l->procs);
        if (!l->stopping && session_has_failed(l->session))
        {
            output_emit_error(l->output, "kernel", S("session append failed; stopping"));
            shutdown_kernel(l, 1);
        }
        if (l->stopping)
            continue;
        moved |= calls_advance(l->calls);
        moved |= turns_advance(l->turns);
    }
}

static bool finished(loop *l)
{
    return turns_is_quiescent(l->turns) &&
           (input_has_ended(l->input) || (l->config->print_answers && turns_has_run(l->turns)));
}

int loop_run(loop *l)
{
    advance(l);
    while (!(l->stopping && proc_count(l->procs) == 0))
    {
        wait_for_io(l);
        advance(l);
        if (!l->stopping && finished(l))
            shutdown_kernel(l, turns_get_exit_code(l->turns));
    }
    return l->exit_code;
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
