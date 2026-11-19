#include "proc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "loop.h"

enum { ABANDON_PIPES_AFTER_KILL_SECONDS = 1 };

static void set_flag(int fd, int cmd_get, int cmd_set, int flag)
{
    fcntl(fd, cmd_set, fcntl(fd, cmd_get) | flag);
}

static void make_pipe(int p[2])
{
    if (pipe(p) < 0) die("pipe: %s", strerror(errno));
    set_flag(p[0], F_GETFD, F_SETFD, FD_CLOEXEC);
    set_flag(p[1], F_GETFD, F_SETFD, FD_CLOEXEC);
}

static void move_fd(int from, int to)
{
    if (from == to) fcntl(to, F_SETFD, 0);
    else dup2(from, to);
}

static void close_fd(int *fd)
{
    if (*fd >= 0) close(*fd);
    *fd = -1;
}

static void exec_child(const spawn_req *r, int stdin_fd, int stdout_fd, int stderr_fd)
{
    signal(SIGPIPE, SIG_DFL);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    setpgid(0, 0);
    signal(SIGINT, SIG_DFL);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    move_fd(stdin_fd, 0);
    move_fd(stdout_fd, 1);
    move_fd(stderr_fd, 2);
    char *path_only[] = { (char *)r->path, NULL };
    char *const *argv = r->argv ? r->argv->v : path_only;
    execvp(argv[0], argv);
    dprintf(2, "%s: %s\n", argv[0], strerror(errno));
    _exit(127);
}

worker *proc_spawn(procs *ps, const spawn_req *r)
{
    int in[2], out[2], err[2];
    make_pipe(in);
    make_pipe(out);
    make_pipe(err);

    sigset_t all, before;
    sigfillset(&all);
    sigprocmask(SIG_BLOCK, &all, &before);
    pid_t pid = fork();
    if (pid == 0) exec_child(r, in[0], out[1], err[1]);
    sigprocmask(SIG_SETMASK, &before, NULL);
    if (pid < 0) die("fork: %s", strerror(errno));
    setpgid(pid, pid);

    close(in[0]);
    close(out[1]);
    close(err[1]);

    worker *w = xcalloc(1, sizeof *w);
    w->pid = pid;
    w->keep_stdin_open = r->keep_stdin_open;
    w->ops = r->ops;
    w->owner = r->owner;
    w->procs = ps;
    w->stdin_fd = in[1];
    w->output_fd[OUT] = out[0];
    w->output_fd[ERR] = err[0];
    for (int i = 0; i < NSTREAM; i++) set_flag(w->output_fd[i], F_GETFL, F_SETFL, O_NONBLOCK);
    set_flag(w->stdin_fd, F_GETFL, F_SETFL, O_NONBLOCK);
    buf_putspan(&w->stdin_pending, r->stdin_bytes);
    if (!w->stdin_pending.n && !w->keep_stdin_open) close_fd(&w->stdin_fd);
    VEC_PUSH(ps->workers, ps->nworkers, ps->capworkers, w);
    return w;
}

int proc_run(const char *path, buf *out)
{
    int in[2], from_child[2], wait_status;
    char chunk[65536];
    make_pipe(in);
    make_pipe(from_child);
    pid_t pid = fork();
    if (pid == 0) exec_child(&(spawn_req){ .path = path }, in[0], from_child[1], 2);
    if (pid < 0) die("fork: %s", strerror(errno));
    close(in[0]);
    close(in[1]);
    close(from_child[1]);
    for (ssize_t n; (n = read(from_child[0], chunk, sizeof chunk)) != 0;)
        if (n > 0) buf_put(out, chunk, (size_t)n);
        else if (errno != EINTR) break;
    close(from_child[0]);
    while (waitpid(pid, &wait_status, 0) < 0)
        if (errno != EINTR) die("waitpid: %s", strerror(errno));
    return wait_status;
}

static void signal_worker(worker *w, int sig)
{
    if (!w->group_empty && kill(-w->pid, sig) < 0 && errno == ESRCH) w->group_empty = true;
    else if (w->group_empty && !w->exited) kill(w->pid, sig);
}

void proc_term(worker *w)
{
    if (!w || w->kill_stage != NOT_KILLED) return;
    signal_worker(w, SIGTERM);
    w->kill_stage = SENT_TERM;
    w->kill_at = monotonic_seconds() + w->procs->config->kill_grace_seconds;
}

void proc_term_all(procs *ps)
{
    for (size_t i = 0; i < ps->nworkers; i++) proc_term(ps->workers[i]);
}

static worker *find_pid(procs *ps, pid_t pid)
{
    for (size_t i = 0; i < ps->nworkers; i++)
        if (ps->workers[i]->pid == pid) return ps->workers[i];
    return NULL;
}

void proc_reap(procs *ps)
{
    int wait_status;
    pid_t pid;
    while ((pid = waitpid(-1, &wait_status, WNOHANG)) > 0) {
        worker *w = find_pid(ps, pid);
        if (!w) continue;
        w->exited = true;
        w->wait_status = wait_status;
        signal_worker(w, 0);
    }
}

static void take_line(worker *w, enum stream s, span line)
{
    if (w->skip_rest_of_line[s]) w->skip_rest_of_line[s] = false;
    else if (line.n > w->procs->config->max_line_bytes) proc_term(w);
    else w->ops->on_line(w, s, line);
}

static void deliver_lines(worker *w, enum stream s, bool eof)
{
    buf *part = &w->partial_line[s];
    char *nl;
    while (part->n && (nl = memchr(part->p, '\n', part->n))) {
        size_t n = (size_t)(nl - part->p);
        take_line(w, s, (span){ part->p, n });
        buf_drop_front(part, n + 1);
    }
    if (eof && part->n) {
        take_line(w, s, buf_span(part));
        part->n = 0;
    }
    if (part->n > w->procs->config->max_line_bytes) {
        take_line(w, s, buf_span(part));
        part->n = 0;
        w->skip_rest_of_line[s] = true;
    }
}

static void feed(worker *w, enum stream s, span bytes, bool eof)
{
    if (w->ops->on_chunk) {
        if (bytes.n) w->ops->on_chunk(w, s, bytes);
        return;
    }
    buf_putspan(&w->partial_line[s], bytes);
    deliver_lines(w, s, eof);
}

static void on_readable(void *ctx, int fd, short revents)
{
    worker *w = ctx;
    char chunk[65536];
    (void)revents;
    for (int s = 0; s < NSTREAM; s++) {
        if (w->output_fd[s] != fd) continue;
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n > 0) feed(w, (enum stream)s, (span){ chunk, (size_t)n }, false);
        else if (n == 0 || errno != EAGAIN) {
            close_fd(&w->output_fd[s]);
            feed(w, (enum stream)s, NOSPAN, true);
        }
    }
}

static void on_writable(void *ctx, int fd, short revents)
{
    worker *w = ctx;
    if (w->stdin_fd != fd) return;
    ssize_t n = (revents & POLLOUT) ? write(fd, w->stdin_pending.p, w->stdin_pending.n) : -1;
    if (n > 0) buf_drop_front(&w->stdin_pending, (size_t)n);
    if ((n < 0 && errno != EAGAIN) || (!w->stdin_pending.n && !w->keep_stdin_open)) {
        close_fd(&w->stdin_fd);
        buf_free(&w->stdin_pending);
    }
}

bool proc_write_stdin(worker *w, span bytes, bool then_close)
{
    if (w->stdin_fd < 0) return false;
    buf_putspan(&w->stdin_pending, bytes);
    if (then_close) w->keep_stdin_open = false;
    if (!w->stdin_pending.n && !w->keep_stdin_open) close_fd(&w->stdin_fd);
    return true;
}

void proc_watch(procs *ps, poll_set *set, bool throttle_output)
{
    for (size_t i = 0; i < ps->nworkers; i++) {
        worker *w = ps->workers[i];
        if (w->stdin_fd >= 0 && w->stdin_pending.n) watch_once(set, w->stdin_fd, POLLOUT, on_writable, w);
        for (int s = 0; s < NSTREAM; s++)
            if (w->output_fd[s] >= 0 && !(s == OUT && throttle_output))
                watch_once(set, w->output_fd[s], POLLIN, on_readable, w);
    }
}

static bool drained(const worker *w)
{
    return w->output_fd[OUT] < 0 && w->output_fd[ERR] < 0;
}

bool proc_deliver_exits(procs *ps)
{
    bool any = false;
    for (size_t i = 0; i < ps->nworkers;) {
        worker *w = ps->workers[i];
        if (!w->exited || !drained(w)) { i++; continue; }
        VEC_REMOVE(ps->workers, ps->nworkers, w);
        close_fd(&w->stdin_fd);
        w->ops->on_exit(w);
        for (int s = 0; s < NSTREAM; s++) buf_free(&w->partial_line[s]);
        buf_free(&w->stdin_pending);
        free(w);
        any = true;
    }
    return any;
}

double proc_next_kill_at(const procs *ps)
{
    double next = 0;
    for (size_t i = 0; i < ps->nworkers; i++) next = earliest_deadline(next, ps->workers[i]->kill_at);
    return next;
}

void proc_escalate_kills(procs *ps, double now)
{
    for (size_t i = 0; i < ps->nworkers; i++) {
        worker *w = ps->workers[i];
        if (!w->kill_at || now < w->kill_at) continue;
        if (w->kill_stage == SENT_TERM) {
            signal_worker(w, SIGKILL);
            w->kill_stage = SENT_KILL;
            w->kill_at = now + ABANDON_PIPES_AFTER_KILL_SECONDS;
        } else {
            for (int s = 0; s < NSTREAM; s++) close_fd(&w->output_fd[s]);
            w->kill_at = 0;
        }
    }
}

size_t proc_count(const procs *ps)
{
    return ps->nworkers;
}

bool proc_succeeded(const worker *w)
{
    return WIFEXITED(w->wait_status) && WEXITSTATUS(w->wait_status) == 0 && w->kill_stage == NOT_KILLED;
}

void proc_describe_status(int wait_status, buf *text)
{
    if (WIFEXITED(wait_status)) buf_printf(text, "exit %d", WEXITSTATUS(wait_status));
    else if (WIFSIGNALED(wait_status)) buf_printf(text, "signal %d", WTERMSIG(wait_status));
    else buf_puts(text, "unknown");
}
