/* loop: the one poll loop all I/O waits in, signals, giving the driver, calls and proc their
   chance to move until nothing does, and when the kernel exits. Holds no conversation state.
   Modules that wait on a descriptor add it to the poll set the loop lends them each round. */

#ifndef LOOP_H
#define LOOP_H

#include "main.h"

typedef void (*ready_fn)(void *ctx, int fd, short revents);

typedef struct { ready_fn fn; void *ctx; } handler;

typedef struct poll_set {
    struct pollfd *fds;
    handler       *handlers;
    size_t         n, cap;
} poll_set;

typedef struct loop {
    poll_set        watched;
    int             signal_fd;
    bool            stopping;
    int             exit_code;
    double          last_sigint_at;
    const config   *config;
    struct driver  *driver;
    struct procs   *procs;
    struct calls   *calls;
    struct ctl     *ctl;
} loop;

void watch_once(poll_set *set, int fd, short events, ready_fn fn, void *ctx);
void loop_open(loop *l);
int  loop_run(loop *l);
void shutdown_kernel(loop *l, int exit_code);

#endif
