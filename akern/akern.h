/* akern.h: what the kernel's modules share (akern.md, design.md). The kernel is one stream of
   lines, the bus, handed out in order to stdout, the session file and five actors, each of which
   moves only when a line reaches it and talks to the others only by putting lines on the bus:

     loop   (loop.c)    the poll loop, signals and timers; stops the kernel on exit
     input  (input.c)   stdin, as commands; the status reply
     tools  (tools.c)   runs the tool calls a reply asks for
     model  (model.c)   the context, and one provider request at a time
     inbox  (inbox.c)   the sequencer: what goes into the conversation, and when

   main.c reads the arguments and wires them; process.c runs child processes for the model and
   the tools. A said line (bus_put) goes everywhere; a told line (bus_tell) is an order between
   modules, to the actors only: command (stdin's, or the positional prompt), eof, exit (code),
   close (after the loop), continue (the model's turn), released (a call no longer holds the
   results back), due (a call's deadline), decide (the inbox, once the lines queued before it
   are seen). */

#ifndef AKERN_H
#define AKERN_H

#include <stdint.h>
#include <sys/types.h>

#include "json.h"

/* The longest stdin or provider line, and the most of a program's result kept in memory: a
   guard against running out of memory, larger than any request's media. */
#define MAX_LINE_BYTES ((size_t)64 << 20)

enum { MAX_ID_CHARS = 32, NEW_ID_CHARS = 8 };

typedef struct config {
    char *session_path, *home_dir, *provider_model, *system_path;
    json_list param_members;
    int max_processes, recursion_limit;
    double kill_grace_seconds;
    bool print_answers;
} config;

/* ---- the loop (loop.c) ---- */

typedef void (*ready_fn)(void *ctx, int fd, short revents);

typedef struct poll_set {
    struct pollfd *fds;
    struct handler *handlers;
    size_t n, cap;
} poll_set;

typedef struct loop {
    poll_set watched;
    int signal_fd, exit_code;
    bool stopping;
    double last_sigint_at;
    struct bus *bus;
    struct input *input;
    struct processes *processes;
} loop;

/* A stream split into lines. */
typedef struct lines {
    buf pending;
    size_t searched; /* how much of pending holds no newline */
    bool skipping;   /* the rest of a line already handed over cut */
} lines;

/* Adds bytes and hands each complete line to fn; at eof, the rest as a line too. A line past
   MAX_LINE_BYTES goes to fn at that length, cut, and the rest of it is dropped. */
void lines_feed(lines *l, span bytes, bool eof, void (*fn)(void *ctx, span line), void *ctx);
/* Adds fd to this round's poll set; fn runs once if it is ready. */
void watch_once(poll_set *set, int fd, short events, ready_fn fn, void *ctx);
/* Creates the signal self-pipe and installs the signal handlers. */
void loop_open(loop *l);
/* Runs rounds until the exit line, then until no process is left; returns the exit code. */
int loop_run(loop *l);
void loop_on_line(void *ctx, json line);
/* Seconds on the monotonic clock, for timeouts and durations. */
double get_monotonic_seconds(void);
/* Wall-clock seconds since the epoch, for timestamps. */
long long get_unix_seconds(void);
/* The earlier of two deadlines, 0 meaning none. */
double earliest_deadline(double a, double b);

/* ---- the bus and the session file (bus.c) ---- */

typedef struct actor {
    void *ctx;
    void (*on_line)(void *ctx, json line);
    void (*put_status)(void *ctx, json_list *members); /* its members of the status reply */
} actor;

typedef struct session {
    int fd, owner_lock_fd;
    bool failed, lost, ends_mid_line;
    uint64_t id_rng;
} session;

typedef struct bus {
    buf queue;   /* lines not yet handed out, each a kind byte, the line, and \n */
    size_t next; /* where the first of them starts */
    struct timer *timers;
    size_t ntimers, captimers;
    const actor *actors;
    size_t nactors;
    session session;
    const config *config;
    buf pending; /* stdout not yet written */
    bool blocking, dead;
    buf answer; /* with -p: the text of the run's last assistant message */
} bus;

/* Opens or creates the session file and takes its owner lock; true if it was empty. */
bool bus_open_session(bus *b);
/* Hands every line of the session file, oldest first, to fn. */
void bus_replay(bus *b, void (*fn)(void *ctx, json line), void *ctx);
/* Starts stdout: every line, or with -p each run's answer. */
void bus_open(bus *b);
/* Queues one line for stdout, the session and the actors. */
void bus_put(bus *b, json line);
void bus_printf(bus *b, const char *fmt, ...);
void bus_put_error(bus *b, const char *where, span message);
/* Queues one line for the actors only. */
void bus_tell(bus *b, json line);
void bus_tellf(bus *b, const char *fmt, ...);
/* Tells a line at a time on the monotonic clock. */
void bus_tell_at(bus *b, double at, const char *fmt, ...);
/* The earliest timer, or 0. */
double bus_next_timer(const bus *b);
/* Queues the lines of the timers that are due. */
void bus_fire_timers(bus *b, double now);
/* Hands out every queued line, those put meanwhile included. */
void bus_drain(bus *b);
/* A new id, 8 hex. */
void bus_new_id(bus *b, char id[NEW_ID_CHARS + 1]);
/* Polls stdout while bytes are pending. */
void bus_watch(bus *b, poll_set *set);
/* True while unwritten stdout is over its cap. */
bool bus_is_backlogged(const bus *b);
/* At exit: hands out what is queued, writes stdout blocking, and gives up the session. */
void bus_close(bus *b);

/* ---- child processes (process.c) ---- */

enum stream { OUT, ERR, NSTREAM };
enum kill_stage { NOT_KILLED, SENT_TERM, SENT_KILL };

typedef struct process process;

typedef struct {
    void (*on_chunk)(process *, enum stream, span bytes);
    void (*on_line)(process *, enum stream, span line);
    void (*on_exit)(process *);
} process_ops;

struct process {
    pid_t pid;
    bool group_empty, exited, keep_stdin_open;
    int wait_status, stdin_fd, output_fd[NSTREAM];
    buf stdin_pending;
    lines output[NSTREAM];
    double kill_at;
    enum kill_stage kill_stage;
    const process_ops *ops;
    void *owner;
    struct processes *all;
};

typedef struct processes {
    process **list;
    size_t n, cap;
    const config *config;
} processes;

typedef struct {
    const char *path;
    const strv *argv;
    span stdin_bytes;
    bool keep_stdin_open;
    bool tool;       /* stderr shares the stdout pipe, as 2>&1, and the second pipe is fd 3 */
    const char *env; /* NAME=VALUE added to the program's environment, or NULL */
    const process_ops *ops;
    void *owner;
} spawn_req;

/* Forks a program in its own process group, with piped stdin, stdout and stderr. */
process *proc_spawn(processes *ps, const spawn_req *r);
/* Before the loop: runs a program to completion, collecting its stdout. */
int proc_run(const char *path, buf *out);
/* Queues bytes for its stdin, then closes it if asked; false if it is closed. */
bool proc_write_stdin(process *p, span bytes, bool then_close);
/* Sends one signal to its process group. */
void proc_signal(process *p, int sig);
/* Once: SIGTERM to the group, then SIGKILL after --kill-grace. */
void proc_term(process *p);
void proc_term_all(processes *ps);
/* After a poll: reaps, escalates kills, and ends the processes that exited with their pipes
   drained. */
void proc_settle(processes *ps, double now);
/* Polls every pipe; with throttle_output, no stdout. */
void proc_watch(processes *ps, poll_set *set, bool throttle_output);
/* The earliest pending SIGKILL deadline, or 0. */
double proc_next_kill_at(const processes *ps);
/* True if it exited 0 and was not killed. */
bool proc_succeeded(const process *p);
/* Writes exit N or signal N for a wait status. */
void proc_describe_status(int wait_status, buf *text);

/* ---- the actors ---- */

typedef struct input {
    lines stdin;
    bool stdin_eof;
    bus *bus;
    const actor *actors;
    size_t nactors;
} input;

void input_watch(input *c, poll_set *set);
void input_on_line(void *ctx, json line);

typedef struct tool_def {
    char *name, *program; /* program is NULL for job, which runs in the kernel */
} tool_def;

typedef struct tools {
    tool_def *defs;
    size_t ndefs, capdefs;
    json_buf schemas; /* sent with every request: each definition without its program */
    struct call **live, **turn;
    size_t nlive, caplive, nturn, capturn;
    int running;
    char *reply_id; /* the latest turn_start's: the reply whose calls these are */
    const config *config;
    bus *bus;
    processes *processes;
} tools;

/* Loads tools.json; dies when there is none, it is malformed, or it names a tool the kernel
   cannot run. */
void tools_open(tools *ts);
void tools_on_line(void *ctx, json line);
void tools_put_status(void *ctx, json_list *members);

typedef struct model {
    process *provider; /* also between turns, while one that stays waits for a request */
    char *provider_path;
    bool provider_reused, provider_spoke;
    buf request_line; /* the turn's request, one line, for a retry on a fresh provider */
    json_buf provider_result, error;
    struct context_entry *context; /* the conversation: message entries since the last reset */
    size_t ncontext, capcontext;
    strv context_ids; /* the ids of the messages the last request sent */
    int block_retries, run, turn;
    char turn_id[NEW_ID_CHARS + 1]; /* the id the turn's reply gets */
    bool turn_open, stopping, retrying;
    buf system;
    const config *config;
    const tools *tools;
    bus *bus;
    processes *processes;
} model;

/* Before the session opens: the provider and the system text, or dies. */
void model_open(model *m);
/* Rebuilds the context from one line of a resumed session. */
void model_replay(void *ctx, json line);
void model_on_line(void *ctx, json line);
void model_put_status(void *ctx, json_list *members);

typedef struct inbox {
    struct item *items;
    size_t nitems, capitems, nadded, mark;
    struct slot *slots;
    size_t nslots, capslots;
    buf *held; /* append commands whose entries wait for a turn boundary */
    size_t nheld, capheld;
    int run, turn;
    bool run_open, turn_open, preempting, turn_failed, abort_pending;
    bool stopping, last_run_failed, settled, eof, deciding;
    char reply_id[NEW_ID_CHARS + 1]; /* the id the open turn's reply gets */
    json_buf turn_error, returned_contents, returned_ids;
    const config *config;
    bus *bus;
} inbox;

void inbox_on_line(void *ctx, json line);
void inbox_put_status(void *ctx, json_list *members);

#endif
