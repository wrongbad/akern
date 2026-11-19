/* proc: every child process the kernel runs, providers and exec programs alike. Owns
   spawning a program in its own process group with piped stdin, stdout and stderr; feeding its
   stdin; reading its output as chunks or lines; reaping it; and stopping it, SIGTERM then
   SIGKILL after --kill-grace. Knows nothing of calls or turns: an owner supplies callbacks and
   learns when its worker is done. Also runs one program to completion, blocking, for use before
   the loop starts.
   worker = one child process; done = exited, with both output pipes at EOF. */

#ifndef PROC_H
#define PROC_H

#include <sys/types.h>

#include "loop.h"

enum stream { OUT, ERR, NSTREAM };
enum kill_stage { NOT_KILLED, SENT_TERM, SENT_KILL };

typedef struct worker worker;

typedef struct {
    void (*on_chunk)(worker *, enum stream, span bytes);
    void (*on_line)(worker *, enum stream, span line);
    void (*on_exit)(worker *);
} worker_ops;

struct worker {
    pid_t  pid;
    bool   group_empty, exited, keep_stdin_open, skip_rest_of_line[NSTREAM];
    int    wait_status;
    int    stdin_fd, output_fd[NSTREAM];
    buf    stdin_pending, partial_line[NSTREAM];
    double kill_at;
    enum kill_stage kill_stage;
    const worker_ops *ops;
    void  *owner;
    struct procs *procs;
};

typedef struct procs {
    worker      **workers;
    size_t        nworkers, capworkers;
    const config *config;
} procs;

typedef struct {
    const char *path;
    const strv *argv;
    span        stdin_bytes;
    bool        keep_stdin_open;
    const worker_ops *ops;
    void       *owner;
} spawn_req;

worker *proc_spawn(procs *ps, const spawn_req *r);
int     proc_run(const char *path, buf *out);
bool    proc_write_stdin(worker *w, span bytes, bool then_close);
void    proc_term(worker *w);
void    proc_term_all(procs *ps);
void    proc_reap(procs *ps);
void    proc_watch(procs *ps, poll_set *set, bool throttle_output);
bool    proc_deliver_exits(procs *ps);
double  proc_next_kill_at(const procs *ps);
void    proc_escalate_kills(procs *ps, double now);
size_t  proc_count(const procs *ps);
bool    proc_succeeded(const worker *w);
void    proc_describe_status(int wait_status, buf *text);

#endif
