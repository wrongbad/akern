/* process: every child process the kernel runs, providers and tool programs alike. Owns
   spawning a program in its own process group with piped stdin, stdout and stderr; feeding its
   stdin; reading its output as chunks or lines; reaping it; signalling its group; and stopping
   it, SIGTERM then SIGKILL after --kill-grace. Knows nothing of tools or the model: an owner
   supplies callbacks and learns when its process is done. Also runs one program to completion,
   blocking, for use before the loop starts.
   process = one child process; done = exited, with both output pipes at EOF. */

#include "akern.h"

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

enum { ABANDON_PIPES_AFTER_KILL_SECONDS = 1 };

static void set_flag(int fd, int cmd_get, int cmd_set, int flag)
{
    fcntl(fd, cmd_set, fcntl(fd, cmd_get) | flag);
}

static void make_pipe(int p[2])
{
    if (pipe(p) < 0)
        die("pipe: %s", strerror(errno));
    set_flag(p[0], F_GETFD, F_SETFD, FD_CLOEXEC);
    set_flag(p[1], F_GETFD, F_SETFD, FD_CLOEXEC);
}

static void move_fd(int from, int to)
{
    if (from == to)
        fcntl(to, F_SETFD, 0);
    else
        dup2(from, to);
}

static void close_fd(int *fd)
{
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
}

static void exec_child(const spawn_req *r, int stdin_fd, int stdout_fd, int stderr_fd, int fd3)
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
    if (fd3 >= 0)
        move_fd(fd3, 3);
    move_fd(stdin_fd, 0);
    move_fd(stdout_fd, 1);
    move_fd(stderr_fd, 2);
    if (r->env)
        putenv((char *)r->env);
    char *path_only[] = {(char *)r->path, NULL};
    char *const *argv = r->argv ? r->argv->v : path_only;
    execvp(argv[0], argv);
    dprintf(2, "%s: %s\n", argv[0], strerror(errno));
    _exit(127);
}

process *proc_spawn(processes *ps, const spawn_req *r)
{
    int in[2], out[2], err[2];
    make_pipe(in);
    make_pipe(out);
    make_pipe(err);

    sigset_t all, before;
    sigfillset(&all);
    sigprocmask(SIG_BLOCK, &all, &before);
    pid_t pid = fork();
    if (pid == 0)
        exec_child(r, in[0], out[1], r->tool ? out[1] : err[1], r->tool ? err[1] : -1);
    sigprocmask(SIG_SETMASK, &before, NULL);
    if (pid < 0)
        die("fork: %s", strerror(errno));
    setpgid(pid, pid);

    close(in[0]);
    close(out[1]);
    close(err[1]);

    process *p = xcalloc(1, sizeof *p);
    p->pid = pid;
    p->keep_stdin_open = r->keep_stdin_open;
    p->ops = r->ops;
    p->owner = r->owner;
    p->all = ps;
    p->stdin_fd = in[1];
    p->output_fd[OUT] = out[0];
    p->output_fd[ERR] = err[0];
    for (int i = 0; i < NSTREAM; i++)
        if (p->output_fd[i] >= 0)
            set_flag(p->output_fd[i], F_GETFL, F_SETFL, O_NONBLOCK);
    set_flag(p->stdin_fd, F_GETFL, F_SETFL, O_NONBLOCK);
    buf_putspan(&p->stdin_pending, r->stdin_bytes);
    if (!p->stdin_pending.n && !p->keep_stdin_open)
        close_fd(&p->stdin_fd);
    VEC_PUSH(ps->list, ps->n, ps->cap, p);
    return p;
}

int proc_run(const char *path, buf *out)
{
    int in[2], from_child[2], wait_status;
    char chunk[65536];
    make_pipe(in);
    make_pipe(from_child);
    pid_t pid = fork();
    if (pid == 0)
        exec_child(&(spawn_req){.path = path}, in[0], from_child[1], 2, -1);
    if (pid < 0)
        die("fork: %s", strerror(errno));
    close(in[0]);
    close(in[1]);
    close(from_child[1]);
    for (ssize_t n; (n = read(from_child[0], chunk, sizeof chunk)) != 0;)
        if (n > 0)
            buf_put(out, chunk, (size_t)n);
        else if (errno != EINTR)
            break;
    close(from_child[0]);
    while (waitpid(pid, &wait_status, 0) < 0)
        if (errno != EINTR)
            die("waitpid: %s", strerror(errno));
    return wait_status;
}

static void signal_group(process *p, int sig)
{
    if (!p->group_empty && kill(-p->pid, sig) < 0 && errno == ESRCH)
        p->group_empty = true;
    else if (p->group_empty && !p->exited)
        kill(p->pid, sig);
}

void proc_signal(process *p, int sig)
{
    signal_group(p, sig);
}

void proc_term(process *p)
{
    if (!p || p->kill_stage != NOT_KILLED)
        return;
    signal_group(p, SIGTERM);
    p->kill_stage = SENT_TERM;
    p->kill_at = get_monotonic_seconds() + p->all->config->kill_grace_seconds;
}

void proc_term_all(processes *ps)
{
    for (size_t i = 0; i < ps->n; i++)
        proc_term(ps->list[i]);
}

static process *find_pid(processes *ps, pid_t pid)
{
    for (size_t i = 0; i < ps->n; i++)
        if (ps->list[i]->pid == pid)
            return ps->list[i];
    return NULL;
}

static void reap(processes *ps)
{
    int wait_status;
    pid_t pid;
    while ((pid = waitpid(-1, &wait_status, WNOHANG)) > 0)
    {
        process *p = find_pid(ps, pid);
        if (!p)
            continue;
        p->exited = true;
        p->wait_status = wait_status;
        signal_group(p, 0);
    }
}

/* A line past MAX_LINE_BYTES stops its process. */
static void take_line(process *p, enum stream s, span line)
{
    if (line.n > MAX_LINE_BYTES)
        proc_term(p);
    else
        p->ops->on_line(p, s, line);
}

static void take_out_line(void *p, span line)
{
    take_line(p, OUT, line);
}

static void take_err_line(void *p, span line)
{
    take_line(p, ERR, line);
}

static void feed(process *p, enum stream s, span bytes, bool eof)
{
    if (p->ops->on_chunk && bytes.n)
        p->ops->on_chunk(p, s, bytes);
    else if (!p->ops->on_chunk)
        lines_feed(&p->output[s], bytes, eof, s == OUT ? take_out_line : take_err_line, p);
}

static void on_readable(void *ctx, int fd, short revents)
{
    process *p = ctx;
    char chunk[65536];
    (void)revents;
    for (int s = 0; s < NSTREAM; s++)
    {
        if (p->output_fd[s] != fd)
            continue;
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n > 0)
            feed(p, (enum stream)s, (span){chunk, (size_t)n}, false);
        else if (n == 0 || errno != EAGAIN)
        {
            close_fd(&p->output_fd[s]);
            feed(p, (enum stream)s, NOSPAN, true);
        }
    }
}

static void on_writable(void *ctx, int fd, short revents)
{
    process *p = ctx;
    if (p->stdin_fd != fd)
        return;
    ssize_t n = (revents & POLLOUT) ? write(fd, p->stdin_pending.p, p->stdin_pending.n) : -1;
    if (n > 0)
        buf_drop_front(&p->stdin_pending, (size_t)n);
    if ((n < 0 && errno != EAGAIN) || (!p->stdin_pending.n && !p->keep_stdin_open))
    {
        close_fd(&p->stdin_fd);
        buf_free(&p->stdin_pending);
    }
}

bool proc_write_stdin(process *p, span bytes, bool then_close)
{
    if (p->stdin_fd < 0)
        return false;
    buf_putspan(&p->stdin_pending, bytes);
    if (then_close)
        p->keep_stdin_open = false;
    if (!p->stdin_pending.n && !p->keep_stdin_open)
        close_fd(&p->stdin_fd);
    return true;
}

void proc_watch(processes *ps, poll_set *set, bool throttle_output)
{
    for (size_t i = 0; i < ps->n; i++)
    {
        process *p = ps->list[i];
        if (p->stdin_fd >= 0 && p->stdin_pending.n)
            watch_once(set, p->stdin_fd, POLLOUT, on_writable, p);
        for (int s = 0; s < NSTREAM; s++)
            if (p->output_fd[s] >= 0 && !(s == OUT && throttle_output))
                watch_once(set, p->output_fd[s], POLLIN, on_readable, p);
    }
}

static bool drained(const process *p)
{
    return p->output_fd[OUT] < 0 && p->output_fd[ERR] < 0;
}

static void deliver_exits(processes *ps)
{
    for (size_t i = 0; i < ps->n;)
    {
        process *p = ps->list[i];
        if (!p->exited || !drained(p))
        {
            i++;
            continue;
        }
        VEC_REMOVE(ps->list, ps->n, p);
        close_fd(&p->stdin_fd);
        p->ops->on_exit(p);
        for (int s = 0; s < NSTREAM; s++)
            buf_free(&p->output[s].pending);
        buf_free(&p->stdin_pending);
        free(p);
    }
}

double proc_next_kill_at(const processes *ps)
{
    double next = 0;
    for (size_t i = 0; i < ps->n; i++)
        next = earliest_deadline(next, ps->list[i]->kill_at);
    return next;
}

static void escalate_kills(processes *ps, double now)
{
    for (size_t i = 0; i < ps->n; i++)
    {
        process *p = ps->list[i];
        if (!p->kill_at || now < p->kill_at)
            continue;
        if (p->kill_stage == SENT_TERM)
        {
            signal_group(p, SIGKILL);
            p->kill_stage = SENT_KILL;
            p->kill_at = now + ABANDON_PIPES_AFTER_KILL_SECONDS;
        }
        else
        {
            for (int s = 0; s < NSTREAM; s++)
                close_fd(&p->output_fd[s]);
            p->kill_at = 0;
        }
    }
}

void proc_settle(processes *ps, double now)
{
    reap(ps);
    escalate_kills(ps, now);
    deliver_exits(ps);
}

bool proc_succeeded(const process *p)
{
    return WIFEXITED(p->wait_status) && WEXITSTATUS(p->wait_status) == 0 &&
           p->kill_stage == NOT_KILLED;
}

void proc_describe_status(int wait_status, buf *text)
{
    if (WIFEXITED(wait_status))
        buf_printf(text, "exit %d", WEXITSTATUS(wait_status));
    else if (WIFSIGNALED(wait_status))
        buf_printf(text, "signal %d", WTERMSIG(wait_status));
    else
        buf_puts(text, "unknown");
}
