# 04 — Kernel implementation sketch

How `akern-loop` is built. The protocols in [protocols.md](protocols.md)
and the model in [concurrency.md](concurrency.md) are normative; this
document is one way to implement them and is free to change.

## Constraints

- C99, POSIX.1-2008. No third-party libraries. **No threads.**
- One translation unit per concern, one `Makefile`, no build generator.
- Hard budget: **3,000 lines** for the kernel. A feature that does not fit is
  a sign it belongs outside.
- Compiles clean with `-Wall -Wextra -Wpedantic -Werror`; tests run under ASan
  and UBSan.
- Linux-only facilities (`PR_SET_PDEATHSIG`) are used behind `#ifdef` as
  backstops, never as the mechanism.

## Source layout

Sources live in one directory per kind of component. Each `.c` file is one
component; the kernel's files are its concerns. Current line counts:

```
src/            the kernel, akern-loop (each .c with a matching .h)
  loop.c        main(), options, the poll loop, timers, signals          292
  driver.c      the conversation state machine, the wake rule, delivery  393
  calls.c       the tools: calls, exec, pending calls, jobs,             557
                writing to a job's stdin
  files.c       read and write for paths: line ranges, media by the      183
                call's type, atomic replace
  inbox.c       queued items, delivery modes                              89
  proc.c        worker table, spawn, process groups, kill escalation,    288
                stdin pipes
  ctl.c         commands from stdin, events to stdout                    335
  session.c     ownership, index, append, chain walk                     252
  msg.c         content blocks and messages                              114
  json.c        span scanner, string decode, writer                      308
  base.c        buffers, spans, string vectors, clocks                   172
providers/      anthropic.c (the only file that links libcurl), mock.c
tools/          the command-line programs: C (akern-edit) and shell (akern)
skills/         akern/SKILL.md, files/SKILL.md
render.jq       how `akern "prompt"` shows events: a jq program, not code
tests/          pytest scenarios (conftest.py fixtures, invariants.py), and bin/ with
                the gate and sleeper test tools; `make smoke` runs them in build/venv
Makefile
```

Every build output goes under `build/`, which is laid out as an `AKERN_HOME`
with the programs alongside:

```
build/
  bin/          akern, akern-loop, akern-edit, and every other akern-* tool
  providers/    anthropic, mock
  skills/       copied from skills/
  system.md     the skills listed, as `make install` writes it to ~/.akern
  render.jq     copied from render.jq
  obj/          object files, mirroring the source tree
  venv/         pytest, for `make smoke`
  asan/         the same layout, built with AddressSanitizer and UBSan
```

Run it in place with `./akern.sh`, which sets `AKERN_HOME=build` and puts
`build/bin` first on `PATH`.

The providers, skills and `tools/` are **userland**. They
live in this repository for convenience and share `json.h`, but the kernel
does not link against them and they could be deleted and rewritten in shell.

## Core data structures

```c
typedef struct { const char *p; size_t n; } span;
typedef span json;                /* the text of one JSON value; NOSPAN when absent */

struct worker {                   /* proc.c: one child process            */
    pid_t   pid;
    bool    own_group, exited, keep_stdin_open;
    int     wait_status;          /* from waitpid                         */
    int     stdin_fd, output_fd[NSTREAM];   /* -1 once closed             */
    buf     stdin_pending;        /* bytes still to write to the child    */
    buf     partial_line[NSTREAM];          /* partial lines read so far  */
    double  kill_at;              /* next kill step, or 0                 */
    enum kill_stage kill_stage;   /* NOT_KILLED, SENT_TERM, SENT_KILL     */
    const worker_ops *ops;        /* on_chunk or on_line; on_exit once
                                     exited AND both pipes at EOF         */
    void   *owner;                /* the call or the driver               */
};

typedef struct {                  /* calls.c: one tool_use block          */
    char       *id, *tool_name;
    json_buf    input;            /* copied from the assistant message    */
    call_state  state;            /* CALL_WAITING, _RUNNING, _DONE, _CANCELLED */
    bool        in_turn;          /* not yet answered                     */
    bool        wait;             /* false: "wait": false in the input    */
    bool        delivers_late;    /* answered with tool_pending: a job    */
    worker     *proc;             /* the tool process                     */
    capture     output[NSTREAM];  /* kept bytes, total, spill file        */
    buf         status;
    json_buf    result; bool is_error;
} call;

typedef struct {                  /* inbox.c: an inbox entry              */
    enum source source;           /* FROM_PROMPT, FROM_JOB, FROM_INJECT   */
    enum when   when;             /* WHEN_NOW, _TURN, _IDLE, _DEFERRED    */
    json_buf    content;
} item;
```

A **worker is done** only when the process has exited *and* its stdout,
stderr have both reached EOF. Waiting for both avoids the classic
bug where `SIGCHLD` arrives before the last of the output has been read.

## The poll loop

```c
while (!(stopping() && proc_count() == 0)) {
    watch_once() each fd: signal self-pipe, stdin,
                    stdout if its backlog is non-empty,
                    each worker (out, err; in if stdin_pending is non-empty)
    timeout = proc_next_kill_at(), or -1
    poll(fds, n, timeout);
    for each ready fd: its handler reads (on_chunk, or split lines → on_line)
                       or flushes its buffer
    proc_reap();                  /* waitpid(-1, WNOHANG) until 0 */
    proc_escalate_kills(now);     /* SIGKILL once --kill-grace has passed */
    until nothing moves: proc_deliver_exits() → on_exit,
                         calls_advance(), driver_advance()
    if quiescent and (stdin closed, or -p and it has run): shutdown_kernel()
}
```

Each worker has 3 descriptors, so 32 workers come to about 100, comfortably within what
`poll(2)` handles. If profiling ever says otherwise, `kqueue`/`epoll` slot in
behind the same loop.

All callbacks only **record** facts (a line arrived, a worker finished, a
command was queued). After `poll`, `advance()` repeats `proc_deliver_exits()`,
`calls_advance()` and `driver_advance()` until none of them moves: calls move
their own state, the driver moves the conversation's. That keeps reentrancy
out of the design: no callback ever runs inside another.

### Backpressure

- Every outbound stream (child stdin, stdout) is non-blocking
  and buffered.
- When the stdout backlog exceeds a fixed cap (8 MiB), the loop stops
  polling provider and tool stdout. Children then block on their own writes,
  so a stalled frontend slows the producers instead of exhausting memory.

## The driver

```c
bool driver_advance(void) {
    if (d.abort_requested) return abort_run();
    if (d.state == S_STREAM && inbox_due(WHEN_NOW)) return preempt();    /* → S_ABORTING */
    switch (d.state) {
    case S_IDLE:     return idle();   /* items due: append_user_message(), begin_turn() */
    case S_STREAM:   return d.provider_exited && streamed();
                     /* append; tool_use ? calls_start() → S_WAITING : settle() */
    case S_ABORTING: return d.provider_exited && aborted();
                     /* append the aborted reply; deliver, begin_turn() (abort: end the run) */
    case S_WAITING:  return (calls_turn_settled() || inbox_due(WHEN_NOW | WHEN_TURN)) && resolved();
                     /* append_user_message(WHEN_NOW | WHEN_TURN, WITH_CALL_RESULTS), begin_turn() */
    }
    return false;
}
```

`begin_turn()` builds the request and spawns the provider (→ `STREAM`).
`settle()` ends a turn without tool calls, or a provider that exited without
a result: due `WHEN_NOW|WHEN_TURN|WHEN_IDLE` items are delivered and a new turn
begins; otherwise the run ends (→ `IDLE`).

`calls_turn_settled()` is the wake rule: every waited call is done or cancelled and
every `wait: false` call has started. `calls_take_turn_results()` gives each call
of the turn a `tool_result` if it is settled and a `tool_pending` otherwise,
marking it `delivers_late`.

`append_user_message()` takes every item due at this point in arrival order
(`inbox_take()`, which pulls in any `WHEN_DEFERRED` items), and builds one
user message (after the tool results, if there are any).

`preempt()` is the only preemption ([concurrency.md](concurrency.md#preemption)):
send `SIGTERM`, move to `ABORTING`, and wait for the provider's `aborted`
result or its grace period to expire. Only then append and rebuild. Without
that wait, the partial message and the steer could be appended in the wrong
order.

## Calls and jobs

- `calls_start()` creates one `call` per `tool_use` block. An `exec` call
  waits (`WAITING`) for a free worker slot (`--max-processes`), then moves to
  `RUNNING`; `read`, `write` and an unknown name finish at once (`DONE`).
- **Output limit**: each stream is kept in memory up to the call's
  `stdout_limit` or `stderr_limit`. The first byte past it opens
  `<session>.d/calls/<call id>.out` (or `.err`), writes what was collected, and
  from then on the stream goes to the file; the result keeps the first bytes
  and adds `stdout_total` and `stdout_file` (or `stderr_…`). stdout with
  `stdout_type` is exempt.
- **Pending**: when the request is built, each unsettled call of the turn
  becomes a `tool_pending` block (its state; pid, file paths and whether stdin
  is open while its process runs) and is marked `delivers_late`. If it has a
  process, `<session>.d/calls/<call id>.out` and `.err` are opened (a stream
  that already spilled keeps its spill file) and the output collected so far
  written; a call still waiting for a slot opens them when it starts. The
  worker is unchanged. A call is a job while it is pending or has `wait`
  false; `write {job}` and `kill` find it by call id.
- **`stdin_open`**: keeps the stdin pipe open after writing any `stdin` text;
  `write {job}` appends to it via `proc_write_stdin()`, which closes it on `close`.
- **`read` and `write`** run in the kernel as soon as their call starts
  (`files.c` for a path, `calls.c` for a job's stdin): no worker, no
  `--max-processes` slot.
- **Job done**: create an inbox item holding one `job_result` block
  (`source: job`, `when: turn`, always). Nothing is written to
  the session until it is delivered.
- **Shutdown**: once every worker has exited, `driver_append_leftover_results()` appends one user
  message with the job results still queued (any `when`) and a `job_result`
  for each pending call that never finished, so no `tool_pending` is left
  open.
- Calls in a job are never referenced by the conversation again, so a job
  can outlive branches, compactions and runs without dangling pointers.

## Session handling

- On open: take the `.owner` lease (`fcntl` `F_SETLK` with `F_WRLCK`; failure
  means another kernel owns the session, which is an error that names its pid). Read
  the file once and build an index of `{id, parent_id, offset, length, is_message}` per
  entry: a flat array plus an open-addressing id table.
- Append: one `write`, then update the index. As sole writer, the kernel never
  has to re-read.
- Context build: walk parents from the head into a small stack, then, in
  reverse, `pread` each line into a temporary buffer and copy its `message`
  member into the request.
- `append` without `parent`: applied immediately with the current head
  as parent. With `parent`: held in a deferred list and applied in
  `settle()` or `IDLE`, before delivery.

## Processes

```c
worker *proc_spawn(const spawn_req *r);  /* path or argv, stdin_bytes, keep_stdin_open,
                                           own_group, ops, owner */
void    proc_term(worker *w);             /* TERM now, KILL after --kill-grace */
```

- `fork`/`exec`. The child sets its process group, resets signal dispositions,
  and dups the pipes to 0/1/2; every other descriptor closes on exec
  (`FD_CLOEXEC`).
- **Tools and providers get their own process group**, so a runaway tree dies
  with one `kill(-pid, …)`.
- Children inherit the environment plus the variables in
  [akern-loop.md §1](akern-loop.md#set-for-children). Every kernel
  descriptor is close-on-exec.
- `SIGPIPE` is ignored; a child that exits early shows up as `EPIPE` on write.

## Options

Flags: [akern-loop.md §2](akern-loop.md#2-arguments).

## Memory

- No arena: each request is built in a plain `buf`, freed once it is copied
  to the provider's stdin; the provider's result sits in one `buf` reused
  every turn.
- Calls, jobs and inbox items are individually allocated, because their
  lifetimes cross turns. They are freed when delivered.
- Buffers grow geometrically up to `--max-line-bytes`. A child that exceeds it is
  killed: a tool call fails, a provider ends its run in error.
  The kernel itself carries on.

## Signals

Effects of `SIGINT`, `SIGTERM` and `SIGHUP`: [akern-loop.md §2](akern-loop.md#exit).
`SIGCHLD` is written to the self-pipe and reaped in the loop.

## Failure handling

| Failure | Result |
|---|---|
| Provider exits without a `result` line | `error` event, run ends with `error` unless inbox items are due; a front end may retry on `run_end` |
| Tool cannot be found or executed | error `tool_result`; the run continues |
| Tool exits non-zero | `is_error: true`; the run continues |
| Provider writes an invalid line | line dropped, `error` event |
| An invalid command on stdin | `error` event (with its `ref`); nothing else changes |
| Session append fails | fatal: `error` event, cascade shutdown, exit 1. The log is the state, so continuing without it would mean the log no longer matches what happened |

The kernel never retries anything. Retrying a model call is the provider's
decision; retrying a run is the front end's.

## Build

One `Makefile`, no build generator. `make` builds `build/` as above;
`make smoke` runs the scenario tests against it; `make asan` builds
`build/asan/` with AddressSanitizer and UBSan and runs the same tests against
it, failing on any sanitizer report (written to `build/asan/sanitizer.*`). The
two trees never share objects, so switching needs no `make clean`. `make lines`
counts the kernel. `make install`
copies every program into `$PREFIX/bin` and the providers into
`~/.akern/providers`, copies skills and `render.jq` into `~/.akern`, and
writes `~/.akern/system.md` listing the skills, without replacing files that
are already there, so an edited `render.jq` or `system.md` survives.

## Testing concurrency

Concurrent behavior is only trustworthy if tests can force each interleaving
on purpose. Three pieces make that possible without a model or a network:

- **`providers/mock.c`** replays a scripted response. It can pause between
  events (`{"x-sleep":0.5}`) or wait until a file exists before continuing, so a
  test can interrupt at an exact point in the stream.
- **`tests/bin/gate PATH`** is a tool that announces itself, then blocks until
  the test writes `PATH`. The test decides
  exactly when each parallel call finishes, so "t2 finishes, a steer arrives
  and t1 is answered pending, then t1 finishes" is a deterministic script.
- **`tests/invariants.py`** checks every session file (sub-agents included) and
  its event log after every test run against the
  [invariants](concurrency.md#invariants): every `tool_use` answered in the
  next message by a `tool_result` or `tool_pending`, in call order; every
  `tool_pending` closed by exactly one `job_result`; no `tool_use` in an
  aborted message; at most one stream open at a time; every `tool_start` has
  a `tool_end` or a result in a message; a `run_end` has an `error` exactly
  when its reason is `error`.

Scenario tests (pytest) drive the gates and assert on the invariants plus a few key events.
They do not compare full transcripts, because those legitimately vary in
interleaving. A **stress target** runs random scenarios (random call counts,
random gate timings, random `send`/`abort`/`kill` at random moments) for N
seconds under ASan, and checks the invariants on every result. `json.c` additionally gets
unit tests and a fuzz target; it is the only code that touches untrusted bytes
structurally.
