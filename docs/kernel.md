# Kernel

The modules of `akern` in `src/`, and the API each offers the others.
What the process reads and writes is [akern.md](akern.md); the
provider API is [providers.md](providers.md); the tools are
[tools.md](tools.md).

Each module is one `.c` with its `.h`. `main.c` holds every module's state in
one root, `struct kernel`, and gives each module pointers to the siblings it
uses. Modules are listed top-down; the graph is [at the end](#dependency-graph).

## Constraints

| Rule | |
|---|---|
| language | C99, POSIX.1-2008 (`-D_XOPEN_SOURCE=700`), `-Wall -Wextra -Wpedantic`; no third-party libraries; no threads |
| files | one source file per real component; never split one into helper files to keep files small |
| platform | Linux-only facilities (`PR_SET_PDEATHSIG`) sit behind `#ifdef`, as backstops, never as the mechanism |
| memory | running out is fatal (exit 2). Calls, jobs and inbox items are allocated one by one, since they outlive turns, and freed once delivered and unreferenced |
| moving | handlers and callbacks record facts, queue items or set flags; the conversation moves only in `driver_advance` (and `driver_stop` at shutdown), and no callback runs inside another |

| Term | Meaning |
|---|---|
| run | from a delivery while idle to `run_end` |
| turn | one provider request and its reply |
| call | one `tool_use` block, from `tool_start` to its answer |
| job | a call answered with `tool_pending`, or started with `wait: false`; named by its call id |
| worker | one child process: the provider or an `exec` program |
| quiescent | idle, with no abort requested, no `now`, `turn` or `idle` item queued and no job running |

## main

`src/main.c`. The entry point and the only code that reads argv and the
environment: options, defaults, and building the kernel.

**State:** `config`, read-only once the kernel runs: `session_path`,
`home_dir`, `provider_model`, `system_path`, `param_members`, `max_processes`,
`kill_grace_seconds`, `max_line_bytes`, `print_answers`, `recursion_limit`.
What `set` changes lives in the driver.

**Points at:** every module.

| Step | |
|---|---|
| options | flags and environment per [akern.md §1–2](akern.md#1-environment-variables); a bad flag, no model or a spent recursion limit exits 2. Without `-s`, `mkstemp` creates `$TMPDIR/akern-session-XXXXXX` |
| wire | every module's sibling pointers and `config` |
| open | `loop_open`, `sink_open`, `session_open`, `driver_open` (exports `AKERN_MODEL`), then exports `AKERN_RECURSION_LIMIT` minus 1 |
| start | `hello` event; the positional prompt as `inbox_add(FROM_PROMPT, WHEN_IDLE)` |
| run | `loop_run` |
| exit | `driver_append_leftover_results`, `sink_flush`, `session_close`; returns the loop's exit code |

## loop

`src/loop.c`. The one poll loop all I/O waits in, signals, and when the kernel
exits. Holds no conversation state.

**State:** the poll set, rebuilt every round; the signal self-pipe;
`stopping`, `exit_code`, `last_sigint_at`.

**Points at:** sink, ctl, driver, calls, procs, session; config.

| Function | Contract | Caller |
|---|---|---|
| `watch_once(set, fd, events, fn, ctx)` | adds `fd` to this round's poll set; `fn(ctx, fd, revents)` runs once if it is ready | `ctl_watch`, `sink_watch`, `proc_watch` |
| `loop_open(l)` | creates the self-pipe; `SIGCHLD`, `SIGINT`, `SIGTERM`, `SIGHUP` are written to it; ignores `SIGPIPE` | main |
| `loop_run(l)` | runs rounds until stopping with no worker left; returns the exit code | main |
| `shutdown_kernel(l, code)` | first call only: sets `stopping` and the code, `driver_stop`, `proc_term_all` | loop |

A round:

1. Poll set: the signal pipe; stdout while a backlog is pending
   (`sink_watch`); stdin unless stopping (`ctl_watch`); every worker's pipes
   (`proc_watch`, its stdout left out while `sink_backlogged`).
2. `poll`, with the earliest kill deadline as timeout (at most 60 s), or none.
3. Ready handlers; `proc_reap`; `proc_escalate_kills`.
4. Until nothing moves: `proc_deliver_exits`; if `session_failed`, an `error`
   event (`kernel`, `session append failed; stopping`) and shutdown with 1;
   unless stopping, `calls_advance` and `driver_advance`.
5. Not stopping, `driver_quiescent`, and `ctl_eof` (or `-p` and
   `driver_ran`): shutdown with `driver_exit_code`.

| Signal | Effect |
|---|---|
| `SIGINT` | `interrupt` event and `driver_abort(KEEP_JOBS)`; while stopping, or within 3 s of the previous one, shutdown with 130 |
| `SIGTERM`, `SIGHUP` | shutdown with 130 |
| `SIGCHLD` | wakes `poll`; reaping is `proc_reap`'s |

## ctl

`src/ctl.c`. The control channel in: commands and prompt lines on stdin.
Replies go out through the sink; what a command does belongs to other modules.

**State:** `stdin_pending` (bytes of an unfinished line), `stdin_eof`,
`skip_rest_of_line` (inside a line over `--max-line-bytes`).

**Points at:** sink, inbox, driver, calls; config; msg.

| Function | Contract | Caller |
|---|---|---|
| `ctl_watch(c, set)` | watches stdin until EOF; each complete line runs at once, in order | loop |
| `ctl_eof(c)` | stdin has ended | loop |

| Line | Calls | Reply |
|---|---|---|
| plain text | `inbox_add(FROM_PROMPT, WHEN_TURN)` | none |
| `send`, `prompt`, `steer` | `inbox_add`, after the `ok` | ctl |
| `abort` | `driver_abort` | ctl |
| `kill` | `calls_kill_job` | ctl |
| `set` | `driver_set` | ctl |
| `append` | `driver_append` | ok: driver, when appended; error: ctl |
| `status` | `driver_status` | ctl |

Line syntax, fields and reply texts: [akern.md §3](akern.md#3-stdin).

## driver

`src/driver.c`. The conversation state machine: runs and turns, one model
request in flight. Owns when queued items are delivered, building each request
and running the provider, every append to the session, preemption, aborts, the
run's outcome, closing every pending call at shutdown, and the provider
settings `set` changes.

**State:** `state`, `run`, `turn`; abort flags (`abort_requested`,
`abort_kills_jobs`, `abort_ends_run`); the provider worker, whether it exited,
its `result` line and `error`; the entry ids of the request's messages
(`context_ids`) and `block_retries`; the run's `returned` contents and answer
text; `last_run_failed`; held appends; `provider_model`, `system_path`,
`param_members`.

**Points at:** sink, calls, inbox, procs, session; config; msg.

| Function | Contract | Caller |
|---|---|---|
| `driver_open(d)` | model, system path and params from `config`; exports `AKERN_MODEL` | main |
| `driver_set(d, model, system_path, params)` | a string `model` is used from the next request and exported as `AKERN_MODEL`; a string `system_path` replaces the path; an object `params` replaces every param. Anything else is ignored | ctl |
| `driver_append(d, type, parent_id, members, ref)` | `AT_HEAD`: appended now. `AS_ROOT` or an existing id: held, and appended at the next idle step or the next turn that ends without tool calls, before anything is delivered. Each append answers `ok` with its `id` when `ref` is given. An unknown id: `false` | ctl |
| `driver_advance(d)` | one step of the state machine; `true` if anything moved | loop |
| `driver_abort(d, jobs)` | requests an abort, acted on at the next step; `KILL_JOBS` also kills every job | ctl, loop |
| `driver_stop(d)` | at shutdown: closes an open run as aborted (below) | `shutdown_kernel` |
| `driver_append_leftover_results(d)` | once every worker is gone: `calls_close`, then one user message (`source: inbox`) with every queued job result, whatever its mode | main |
| `driver_status(d, out)` | the `status` event object | ctl |
| `driver_quiescent(d)` | idle, no abort requested, nothing due (`now`, `turn`, `idle`), no job running | loop |
| `driver_ran(d)` | a run has started | loop |
| `driver_exit_code(d)` | 1 if the last run ended `error`, else 0 | loop |

### States

| State | Meaning | Leaves when |
|---|---|---|
| `idle` | no run open | an item is due (`now`, `turn`, `idle`): `run_start`, deliver, begin a turn → `stream` |
| `stream` | the provider runs | a `now` item is due: `preempt` event, `SIGTERM` to the provider → `aborting`. The provider exits: without a `result`, omit a rejected block (below) and begin a turn, or else an error and settle; with one, append it; `stop: tool_use` with `tool_use` blocks: `calls_start`, `inbox_mark` → `waiting`; otherwise `turn_end` and settle |
| `aborting` | the provider has had `SIGTERM` | the provider exits: its reply, without `tool_use` blocks, is appended with `meta.stop: aborted` if anything remains. After an abort: `run_end` `aborted` → `idle`. After a preemption: deliver `now` and `turn` items, begin a turn → `stream` |
| `waiting` | the turn's calls run | `calls_turn_settled`, or a `now` or `turn` item queued since `inbox_mark`: deliver with the call answers, `turn_end`, begin a turn → `stream` |

| Step | Does |
|---|---|
| deliver | appends one user message: the call answers (`calls_take_turn_results`, in `waiting` only), then `inbox_take` of the modes due. `source` is `results`, `inbox` or `results+inbox`; nothing to deliver, no message |
| begin a turn | `turn` + 1; the request ([providers.md §2](providers.md#2-request)): `session_context` (keeping the messages' entry ids), the system file read now, `calls_tool_schemas`, params; spawn the provider (none found: error, `run_end` `error`); `turn_start` |
| omit a rejected block | the provider's `error` has `kind: invalid_block` and an `at` ([providers.md §4](providers.md#4-errors)), fewer than 8 have been omitted since the run began or a provider last gave a `result`, `at.message` is an index into the request's messages, and `message_omit_media` finds an image or document there in the entry's current message: append `replace` (`target` the entry, `message` the result) and a `replace` event. Otherwise it is an ordinary failure |
| settle | a turn ended without tool calls, or the provider failed: held appends; any `now`, `turn` or `idle` item due → deliver, begin a turn; otherwise `run_end` (`done`, or `error` after a failure) → `idle`, and `sink_answer` with the run's last assistant text |

A `waiting` turn wakes only for items queued after it began waiting: a line
typed while the model streamed waits for the calls and goes with their results.
So `exec` never holds a turn hostage: anything that needs the model answers
its unfinished calls pending.

### Abort and stop

| | `driver_abort` (next step) | `driver_stop` (shutdown) |
|---|---|---|
| jobs | with `KILL_JOBS`, `calls_kill_jobs` | the loop terminates every worker |
| run open | queued `turn` and `idle` user items go to `run_end.returned`; queued job results become `deferred`. `now` items stay queued and start the next run | the same |
| `stream` | `SIGTERM` to the provider → `aborting`; its partial reply is appended | the streaming reply is discarded |
| `waiting` | `calls_abort_turn`, then a user message of the call answers (`wait: false` calls answered pending) | the same |
| end | `run_end` `aborted` | the same |
| no run | only `jobs` acts | nothing |

### Guarantees

- One provider at a time per session.
- Every call of an assistant message is answered in the next user message,
  before the next request.
- Every `tool_pending` is closed by one `job_result`: delivered through the
  inbox, or by `driver_append_leftover_results` at exit.
- The driver is the only caller of `session_append`. Every appended message is
  also a `message` event, and every `replace` it appends a `replace` event, so a
  front end can rebuild exactly what the model saw.
- Preemption, aborts and branches only add entries.

### Sequences

`→` marks the events a front end sees.

`exec(npm run dev)` with `wait: false` and `read(src/app.ts)` together:

```
result [tool_use t1 exec, tool_use t2 read]               → message, tool_start t1, tool_start t2
t2 runs in the kernel                                     → tool_end t2
t1 has started: the turn is settled
user [tool_pending t1 running, tool_result t2]            → message, turn_end, turn_start
t1 exits 0; inbox(job, turn)                              → tool_end t1, queued
the next turn ends without tool calls; settle:
  user [job_result t1]                                    → message, turn_start
```

`send {when: now}` while the model streams:

```
user: send {"when":"now","content":"use snake_case"}      → ok, queued
SIGTERM to the provider                                   → preempt
result stop=aborted, text only
append assistant [text so far]                            → message
append user [text "use snake_case"]; next turn            → message, turn_start
```

A line typed while `t5` runs and `t6` waits for a slot:

```
"skip the migration"                                      → queued
user [tool_pending t5 running, tool_pending t6 waiting,
      text "skip the migration"]                          → message, turn_end, turn_start
a slot frees; t6 runs and exits                           → tool_end t6, queued
user [job_result t6] when the driver next delivers        → message
```

A job finishes while idle:

```
t3 exits; inbox(job, turn)                                → tool_end t3, queued
idle with a turn item due                                 → run_start
user [job_result t3]; request                             → message, turn_start
```

## calls

`src/calls.c`. The model's tools ([tools.md §1](tools.md#1-built-in-tools))
and every call from its `tool_use` block to its answer: running `exec`
programs within `--max-processes`, keeping and spilling their output, writing
to a job's stdin, killing jobs, answering the turn's calls, and queueing a
job's `job_result`.

**State:** the live calls (every call not yet freed); the turn's calls, in
order; the count of running `exec` programs; the tool schemas. Per call: id,
name, input, run, turn, state, `in_turn`, `wait`, `delivers_late` (answered
pending), `killed`, its worker, output captures, status and result.

**Points at:** sink, procs, inbox; config; files, msg.

```
 waiting ──► running ──► done
 (for a      (exec
  slot)      process)
    └───────────┴──► cancelled (abort)
```

`read`, `write`, an unknown name and a non-object input skip `waiting` and
`running`: they are `done` as soon as the call starts, with no worker and no
slot.

| Function | Contract | Caller |
|---|---|---|
| `calls_tool_schemas(cs)` | the `tools` array of every request | driver |
| `calls_start(cs, message, run, turn)` | one call per `tool_use` block, in order, each with a `tool_start` event; returns the turn's call count | driver |
| `calls_advance(cs)` | starts waiting `exec` calls while fewer than `--max-processes` run; collects exited ones; `true` if anything moved. The limit counts `exec` programs only, so the provider always starts and the conversation cannot deadlock behind its own jobs | loop |
| `calls_turn_settled(cs)` | every waited call is `done` or `cancelled`, and every `wait: false` call has left `waiting` | driver |
| `calls_abort_turn(cs)` | the turn's unfinished waited calls: `waiting` → `cancelled`, `Not run: the run was aborted.`; `running` → terminated, `Killed: the run was aborted.` (`tool_end` status `killed`). `wait: false` calls are left alone | driver |
| `calls_take_turn_results(cs, blocks)` | per call of the turn, in order: a `tool_result` if settled, else a `tool_pending`, which makes it a job and opens its output files. Empties the turn | driver |
| `calls_kill_job(cs, id)` | a job still `waiting` never starts: `Not run: killed before it started.`, status `not run`; one with a process is terminated, `killed: true`. `false` when no live job has that id | ctl |
| `calls_kill_jobs(cs)` | terminates every job that has a process | driver |
| `calls_running_jobs(cs)` | jobs not yet `done` or `cancelled` | driver |
| `calls_turn_json(cs, out)`, `calls_jobs_json(cs, out)` | the `calls` and `jobs` arrays of `status` | driver |
| `calls_close(cs)` | at exit, with every worker gone: each pending call gets the result its exit gave, or `Not run: the kernel stopped.` (`ran` 0) if it never started | driver |

### Jobs

| Rule | |
|---|---|
| becoming one | `wait: false` makes a call a job from the start; any call answered `tool_pending` becomes one. A `wait: false` call that finished before its turn was answered gets an ordinary `tool_result` |
| output | kept in memory up to the call's limit; the first byte past it opens `<session>.d/calls/<call>.out` (or `.err`) and the rest goes there too. From the `tool_pending` on (or its start, if it was still waiting), both streams go to their files. Kept stdout and stderr together over `--max-line-bytes`: terminated |
| end | `tool_end`, then one `job_result` block queued with `inbox_add(FROM_JOB, WHEN_TURN)`, always. Nothing is written to the session until it is delivered |
| `tool_end` status | `done`, `killed`, `cancelled` (not started before an abort), `not run` (killed before it started, or not started when the kernel stopped) |
| `job_result` status | `exit N`, `signal N`, `killed`, `not run` |
| lifetime | independent of turns and runs: until it exits, is killed (`kill -TERM -- -PID`, the `kill` command, `abort` with `jobs`) or the kernel stops. An answered call is never referenced by the conversation again, so a job survives branches, compactions and runs |
| stdin | `write {job}` goes to `proc_write_stdin`; `write {path}` and `read` to files |

## files

`src/files.c`. `read` and `write` on paths, the two tools that run inside the
kernel and finish at once. Each turns a call's input into a result's content
and whether it failed; behavior and error texts:
[tools.md](tools.md#read).

**State:** none. **Points at:** msg, json.

| Function | Contract | Caller |
|---|---|---|
| `tool_read(input, result)` | a text file by line range, or any file as an image or document block given `type` | calls |
| `tool_write_file(input, result)` | atomic replace, or append | calls |

## proc

`src/proc.c`. Every child process, providers and `exec` programs alike:
spawning in its own process group with piped stdin, stdout and stderr; feeding
stdin; reading output as chunks or lines; reaping; stopping. Knows nothing of
calls or turns: an owner supplies callbacks.

**State:** `procs`, the worker table. Per worker: pid, stdin pipe and the
bytes still to write, output pipes and partial lines, exit status, kill stage
and deadline, `ops`, `owner`.

**Points at:** config; loop's `poll_set` and `watch_once` only.

```c
typedef struct {
    void (*on_chunk)(worker *, enum stream, span bytes);  /* raw output (exec) */
    void (*on_line)(worker *, enum stream, span line);    /* split lines (provider) */
    void (*on_exit)(worker *);                            /* once, when done */
} worker_ops;
```

A worker is **done** when it has exited *and* both output pipes are at EOF, so
no output is lost to an early `SIGCHLD`.

| Function | Contract | Caller |
|---|---|---|
| `proc_spawn(ps, req)` | forks `req.path` (no arguments) or `req.argv` (`execvp`) in a new process group, signals reset, every other descriptor closed on exec, `SIGTERM` on parent death on Linux. stdin gets `stdin_bytes`, then closes unless `keep_stdin_open`. A failed `exec` writes the reason to stderr and exits 127 | driver, calls |
| `proc_write_stdin(w, bytes, close)` | queues bytes; `close` closes the pipe once they are written. `false` if stdin is already closed | calls |
| `proc_term(w)` | once: `SIGTERM` to the group, `SIGKILL` after `--kill-grace` | driver, calls |
| `proc_term_all(ps)` | `proc_term` for every worker | `shutdown_kernel` |
| `proc_reap(ps)` | `waitpid` for every exited child | loop |
| `proc_watch(ps, set, throttle)` | polls every pipe; with `throttle`, no stdout | loop |
| `proc_deliver_exits(ps)` | `on_exit` for each done worker, which is then freed; `true` if any | loop |
| `proc_next_kill_at(ps)`, `proc_escalate_kills(ps, now)` | the next deadline; at it, `SIGKILL`, and 1 s later the pipes are abandoned, so a descendant holding them cannot keep a worker alive | loop |
| `proc_count(ps)` | workers not yet done | loop |
| `proc_succeeded(w)` | exited 0 and never signalled by the kernel | calls |
| `proc_describe_exit(w, text)` | `exit N` or `signal N` | calls |

| Rule | |
|---|---|
| signals | go to the process group, so they reach everything a program started; once the group is gone, to the pid alone |
| lines | an `on_line` worker whose line exceeds `--max-line-bytes` is terminated; the line is dropped through its newline |
| every child dies | each is reaped, or killed, before the kernel exits. A sub-agent's tools are in their own groups under the child kernel, which stops them in its own shutdown |
| `SIGKILL` of the kernel | cannot cascade. Linux: `PR_SET_PDEATHSIG`. macOS: a worker's stdin is a pipe from the kernel, so a well-behaved tool sees EOF; orphans remain possible ([roadmap.md](roadmap.md#orphans-after-the-kernel-is-killed-on-macos)) |

## inbox

`src/inbox.c`. What waits to enter the conversation: prompts and injected
content from the front end, and finished jobs' results. Queues items and hands
them out by delivery mode, in arrival order.

**State:** items (`source`, `when`, `seq`, `content`); `nadded`; `mark`.

**Points at:** sink; msg.

### Delivery modes

| `when` | Delivered | If no run is open |
|---|---|---|
| `now` | as `turn`; also preempts a streaming response | starts a run |
| `turn` | at the next delivery: after a streaming turn ends (with its call answers), or at once when the turn waits on calls, which are then answered pending | starts a run |
| `idle` | when a turn ends without tool calls, before the run ends | starts a run |
| `deferred` | with the next delivery of anything else | stays queued |

| `source` | From | Mode |
|---|---|---|
| `prompt` | a plain line (`turn`), the positional prompt (`idle`), `send`, `prompt`, `steer` | as sent |
| `inject` | `send` with `source: inject` | as sent |
| `job` | a finished job | always `turn`; `abort` turns those already queued `deferred` |

Every finished job wakes the agent: no option, global or per call, defers or
drops a result. Everything due at one moment goes into one user message in
arrival order, so ten jobs finishing together wake the model once.

| Function | Contract | Caller |
|---|---|---|
| `when_parse(name, when)`, `when_name(when)` | between `"now"`… and `enum when` | ctl, inbox |
| `inbox_add(in, source, when, content)` | queues a string or block array; `queued` event | ctl, calls, main |
| `inbox_due(in, mask)` | any item whose mode is in `mask` | driver |
| `inbox_mark(in)` | items queued from now on are "since the mark" | driver |
| `inbox_due_since_mark(in, mask)` | `inbox_due`, counting only items since the mark | driver |
| `inbox_take(in, mask, blocks)` | removes every item in `mask`, plus every `deferred` item when `mask` is not empty, in arrival order; appends their blocks (a string becomes a text block); returns the count | driver |
| `inbox_return_prompts(in, mask, contents)` | removes `prompt` and `inject` items in `mask`; appends their contents | driver |
| `inbox_defer_jobs(in)` | every queued job result becomes `deferred` | driver |
| `inbox_take_jobs(in, blocks)` | removes every job result, whatever its mode | driver |
| `inbox_size(in)` | queued items | driver |

## session

`src/session.c`. The session log, which this kernel alone appends to while it
runs: the ownership lease, appends, the head, the context, and whether an
append has failed. File format: [akern.md §5](akern.md#session-file).

**State:** the file and its size; the `.owner` lock; an index of every entry
(`id`, `parent`, offset, length, whether it is a `message`, and the last
`replace` entry that targets it) with an open-addressing id table; the head id;
`failed`.

**Points at:** config; json.

| Function | Contract | Caller |
|---|---|---|
| `session_open(s)` | opens or creates the file (mode 0644); takes a `fcntl` write lock on `S.owner` (held by another process: exit 2, naming its pid) and writes `{"pid":N}` into it; indexes every line; writes the header if the file is empty | main |
| `session_close(s)` | empties `S.owner`; the lock goes with the process | main |
| `session_head_id(s)` | the head's id; `""` for an empty session | main, driver |
| `session_append(s, type, parent_id, members)` | one `write` of one line, `{type, id, parent, ts, …members}`, with a new 8-hex id; `AT_HEAD` uses the head as parent, `AS_ROOT` `null`. Returns the new head id. A failed write sets `failed`; from then on every append returns `NULL` | driver |
| `session_has_entry(s, id)` | the id is indexed | driver |
| `session_failed(s)` | an append has failed | loop |
| `session_context(s, messages, ids)` | the current message of every `message` entry from the head back to a root, oldest first, read with `pread`; each entry's id is pushed to `ids` | driver |
| `session_message(s, id, message)` | the current message of one `message` entry; `false` for any other id | driver |

| Rule | |
|---|---|
| one writer | while it runs, only this kernel appends; others go through `append` on its stdin |
| append-only | never rewritten or truncated; as sole writer it never re-reads, only updates the index |
| reading | the first line (the header), lines that are not valid JSON and lines without a string `id` are not entries; ids up to 32 characters |
| current message | a `message` entry's own, or that of the last `replace` entry in the file whose `target` is it and whose `message` is an object, whatever its chain ([akern.md §5](akern.md#session-file)) |
| chain | stops at a `parent` that is `null` or unknown, or after as many steps as there are entries |
| failure | after a failed append the loop shuts down with exit 1: the log is the state, and running on without it would make it disagree with what happened |

## sink

`src/sink.c`. Everything the kernel writes to stdout: the event stream or,
with `-p`, each run's answer, with a failed run's error on stderr. Knows
nothing of the modules that write to it.

**State:** pending bytes; `blocking` (stdout is a terminal, or after
`sink_flush`); `dead`; `answers_only`.

**Points at:** loop's `poll_set` and `watch_once` only.

| Function | Contract | Caller |
|---|---|---|
| `sink_open(out, answers_only)` | stdout non-blocking unless it is a terminal | main |
| `sink_watch(out, set)` | polls stdout while bytes are pending | loop |
| `sink_backlogged(out)` | more than 8 MiB pending; the loop then stops reading workers' stdout | loop |
| `sink_flush(out)` | blocking write of everything pending | main |
| `sink_answer(out, answer, error)` | with `-p`: the answer and `\n` on stdout (nothing when empty); `akern: MESSAGE` on stderr when `error` is given | driver |
| `event(out, fmt, …)` | one event line, `json_printf` format | every module |
| `event_raw(out, line)` | one JSON line as given | driver, ctl |
| `event_notice(out, source, message)` | a `notice` event | driver |
| `event_error(out, where, message)` | an `error` event | driver, loop |

With `answers_only`, events are dropped. A write error or hang-up on stdout
marks it dead: output is discarded and the kernel carries on.

## msg

`src/msg.c`. The shape of conversation content
([providers.md §5](providers.md#5-messages)): building blocks and user
messages, and the few things the kernel reads in a message. Every other part
of a message is opaque.

**State:** none. **Points at:** json.

| Function | Contract | Caller |
|---|---|---|
| `blocks_add_content(blocks, content)` | a string as one text block, or an array's blocks as they are | inbox |
| `blocks_add_text(blocks, text)` | a text block; empty text becomes `(empty)` | calls, msg |
| `blocks_add_tool_result(blocks, id, is_error, content)` | a `tool_result` block | calls |
| `blocks_add_media(blocks, media_type, name, bytes)` | an `image` block for `image/*`, else a `document` block, base64; a non-NULL `name` goes on a `document` | files, calls |
| `content_from_text(content, text)` | a one-text-block array | files, calls |
| `content_split_text(content, text, other)` | the text blocks' text, joined, and the other blocks | driver |
| `content_valid(content)` | valid JSON, and a string or an array | ctl |
| `message_user(message, blocks)` | `{"role":"user","content":[…]}` | driver |
| `message_strip_tool_use(message, stripped)` | an assistant message without its `tool_use` blocks; `false` when nothing remains | driver |
| `message_empty(message)` | no content blocks | driver |
| `message_omit_media(message, block, inner, reason, out)` | the message with content block `block` (or block `inner` of its `content`, when `inner` ≥ 0) swapped for `{"type":"text","text":"[image omitted: REASON]"}` (`document` for a document); `false` when that block is not an image or document | driver |

## json

`src/json.c`, shared with the providers. All JSON handling, and the only code
that takes untrusted bytes apart. A value read is a slice of the original
bytes, never a parsed copy, so what the kernel stores or resends is byte for
byte what it received.

**State:** none. **Points at:** base.

| Type | |
|---|---|
| `json` | a `span`: the text of one value, still escaped; `NOSPAN` when absent |
| `json_buf` | builds the text of one value |
| `json_list` | builds comma-separated elements or members, without brackets |

| Function | Contract |
|---|---|
| `json_trim(v)`, `json_valid(text)`, `json_is(v, c)`, `json_null_or_absent(v)` | whitespace trimmed; the whole text is one valid value (nesting up to 256); the value starts with `c`; `null` or absent |
| `json_get(object, key)`, `json_iter(container)`, `json_next(it, key, value)` | a member, or `NOSPAN`; iterate an object's members or an array's elements |
| `json_eq(v, text)`, `json_text(string, buf)`, `json_text_dup(string)` | a string equal to `text` once decoded; decode a string (`false` / `NULL` if not a string) |
| `json_escaped_body(string)` | the text between a string's quotes, still escaped; empty if not a string |
| `json_num(v, fallback)`, `json_true(v)` | a number, or a string holding one; the literal `true` |
| `json_put_string(out, text)` | a JSON string; invalid UTF-8 becomes U+FFFD |
| `json_comma_unless_first(array)` | a `,` unless the array being built has just opened |
| `json_printf(out, fmt, …)`, `json_vprintf` | `'` writes `"`; `%s` C string and `%S` span as strings or `null`; `%R` raw JSON or `null`; `%r` raw C string; `%d` int; `%D` long long; `%f` double; `%b` bool |

Callers: every module but loop, proc and base.

## base

`src/base.c`, shared with the providers. Memory, byte buffers, spans, string
vectors, complete writes and clocks. A `char *` returned (except by
`buf_cstr`) or held in a struct is owned; a `const char *` is borrowed.

**State:** none. **Points at:** nothing.

| Function | Contract |
|---|---|
| `xmalloc`, `xcalloc`, `xrealloc`, `xstrdup` | allocate; out of memory is fatal |
| `die(fmt, …)` | `akern: MESSAGE` on stderr, exit 2 |
| `write_all(fd, bytes)` | writes everything, retrying on `EINTR`; `false` on error |
| `S`, `span_eq`, `span_dup` | spans |
| `buf_put`, `buf_puts`, `buf_putc`, `buf_putspan`, `buf_printf`, `buf_set`, `buf_drop_front`, `buf_span`, `buf_cstr`, `buf_free` | growable byte buffers |
| `strv_push`, `strv_free`, `VEC_PUSH`, `VEC_REMOVE` | string vectors and pointer arrays |
| `monotonic_seconds`, `unix_seconds`, `earliest_deadline` | clocks; the earlier of two deadlines, `0` meaning none |

## Dependency graph

```
main ─► loop ─► ctl ─► driver ─► calls ─► files
                                   │
                                   ▼
                        procs · inbox · session ─► sink

libraries: msg ─► json ─► base        config: written by main, read by all
```

Arrows may skip layers (loop also calls driver and procs; driver also calls
procs, inbox and session); the table lists every edge.

| Module | Points at |
|---|---|
| main | every module |
| loop | sink, ctl, driver, calls, procs, session |
| ctl | sink, inbox, driver, calls, msg |
| driver | sink, calls, inbox, procs, session, msg |
| calls | sink, procs, inbox, files, msg |
| files | msg, json |
| proc | loop's `poll_set` type and `watch_once` |
| inbox | sink, msg |
| session | json |
| sink | loop's `poll_set` type and `watch_once` |
| msg | json |
| json | base |

The table is in module order, and each module points only at modules below
it, except proc and sink, which take loop's poll-set type and `watch_once`,
never its state.
