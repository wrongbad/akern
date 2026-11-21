/* loop: the one poll loop all I/O waits in, signals, giving turns, calls and proc their
   chance to move until nothing does, and when the kernel exits, including when the session
   can no longer be appended to, and the clocks every timeout and timestamp is read from.
   Holds no conversation state.
   Modules that wait on a descriptor add it to the poll set the loop lends them each round. */

#ifndef LOOP_H
#define LOOP_H

#include "main.h"

typedef void (*ready_fn)(void *ctx, int fd, short revents);

typedef struct {
    ready_fn fn;
    void *ctx;
} handler;

typedef struct poll_set {
    struct pollfd *fds;
    handler *handlers;
    size_t n, cap;
} poll_set;

typedef struct loop {
    poll_set watched;
    int signal_fd;
    bool stopping;
    int exit_code;
    double last_sigint_at;
    const config *config;
    struct output *output;
    struct input *input;
    struct turns *turns;
    struct calls *calls;
    struct procs *procs;
    struct session *session;
} loop;

/* Adds fd to this round's poll set; fn runs once if it is ready. */
void watch_once(poll_set *set, int fd, short events, ready_fn fn, void *ctx);
/* Creates the signal self-pipe and installs the signal handlers. */
void loop_open(loop *l);
/* Runs rounds until stopping with no worker left; returns the exit code. */
int loop_run(loop *l);
/* First call only: starts shutting down with this exit code. */
void shutdown_kernel(loop *l, int exit_code);

/* Seconds on the monotonic clock, for timeouts and durations. */
double get_monotonic_seconds(void);
/* Wall-clock seconds since the epoch, for timestamps. */
long long get_unix_seconds(void);
/* The earlier of two deadlines, 0 meaning none. */
double earliest_deadline(double a, double b);

#endif
