# Roadmap

Work not yet done, and decisions still open. What exists is specified in
[akern.md](akern.md), [kernel.md](kernel.md),
[providers.md](providers.md) and [tools.md](tools.md); why, in
[design.md](design.md). Everything below is userland unless it says otherwise.

## Work

### Front end

`akern-cli` ([tools.md §4](tools.md#4-the-akern-cli-front-end)) sends one prompt per
line and renders events through `render.jq`.

| Item | Scope |
|---|---|
| sessions | a path per directory, `~/.akern/sessions/<cwd>/<time>-<id>.jsonl`; `--continue` and `--resume` pass `-s` |
| settings | [where settings live](#where-settings-live); the default model is done (`$AKERN_HOME/model`, `akern-model`) |
| context files | `AGENTS.md` / `CLAUDE.md` found from the working directory, merged into the `-S` file |
| prompt input | `/name` expands `prompts/<name>.md`; `@file` becomes content blocks; `!cmd` sends a command's output, `!!cmd` only shows it |
| slash commands | `/cmd args` runs `akern-cmd SESSION args` and shows its stdout ([tools.md §2](tools.md#2-programs-on-path)) |
| interrupt key | sends the line typed with `when: now` |
| auto-compaction | on `run_end` with `context_overflow`, or when `meta.usage` nears the model's context: `akern-compact`, then `send` to continue |
| TUI | a separate full-screen program on the same stream, showing parallel calls, jobs and the sub-agent tree |

### Commands

Session commands follow [tools.md §2](tools.md#2-programs-on-path): the
session path first, direct appends only to an unowned session.

| Command | Does |
|---|---|
| `akern-sessions` | lists sessions: path, name, head, last activity |
| `akern-fork` | copies a chain to a new file |
| `akern-tree` | shows the entry tree; appends a `branch` |
| `akern-compact` | runs the provider for a summary, then appends a new root and the kept copies ([akern.md](akern.md#session-file)) |
| `akern-models` | runs each provider in `$AKERN_HOME/providers` with `--models` |
| `akern-pkg` | `install`, `remove`, `update`, `list`: programs onto `PATH`, providers and skills into `$AKERN_HOME` |

### AKERN_ASK helper

A shipped program for the [`AKERN_ASK`](tools.md#akern_ask) convention: asks on
`/dev/tty` under a lock, so questions from parallel calls queue. The front end
exports it.

### Providers

Compare two or three wordings of `tool_pending` and `job_result`
(`provider_describe_pending`, `provider_describe_job`) on long-running tasks:
does the model wait, poll in a loop, or forget the job.

### Examples

Published, not shipped: an MCP bridge command with its skill, and
`akern-export` (HTML from the JSONL).

### Tests

Once the code stops churning:

| Test | |
|---|---|
| json | unit tests and a fuzz target for `src/json.c`, the only code that takes untrusted bytes apart |
| stress | random scenarios under ASan, checked by `tests/invariants.py`, for 10 minutes |
| finish orders | every finish order of three parallel calls, driven by `tests/bin/gate` |
| cascade | shutdown through three levels of sub-agents |

## Open questions

### Exec wrapper

Whether the kernel runs every `exec` through a wrapper (`--exec-via PROG`:
argv becomes `PROG argv…`), so one program can sandbox, confirm, log or
forward calls (a container, `ssh`). Today a shim on `PATH` wraps one program,
and only an OS sandbox around the whole kernel covers everything
([design.md](design.md#no-interception-points)).

Tied to it: how `read` and `write`, which run in the kernel, would be
intercepted.

| Option | Cost |
|---|---|
| no flag; whole-kernel sandbox and `PATH` shims | `read` and `write` pass no policy point |
| `--exec-via` for `exec` only | `read` and `write` still need the sandbox |
| `--exec-via`, with `read` and `write` run as programs through it | a process per file call; media and atomic replace move out of the kernel |

### Event set

Deferred; leave as is for now. Candidates:

- drop the `status` command and event, `queued`, `run_start` and `tool_start`;
- add `meta` to the `message` event;
- pair `turn_start` and `turn_end`: today `turn_end` is not sent for a turn
  that was preempted, aborted or whose provider failed.

### Appending to an owned session

Only the kernel appends while it owns a session; others go through whoever
drives it, with `append` on its stdin. Whether other programs (a checkpointer,
a second UI) get a way in.

| Option | Cost |
|---|---|
| none: the front end forwards | every writer needs the front end's cooperation |
| a relay program that owns the kernel's stdin and accepts appends from others | one more process; no kernel change |
| the kernel accepts `flock`ed appends while it owns the file | it must re-read the tail before each request; it is no longer the sole writer |

### Where settings live

The kernel reads flags and environment only. The default model already has a
home: `$AKERN_HOME/model`, one line, written by `akern-model` and turned into
`AKERN_MODEL` by the `akern-cli` front end ([tools.md §4](tools.md#4-the-akern-cli-front-end)).
Still open: the other settings (`-P` params such as effort, the system prompt,
limits), and whether each gets a one-value file like `model` or they share a
`~/.akern/config`. *Leaning:* the `akern-cli` front end, with `key=value` lines that
a shell reads without a parser, so front ends in any language share it.

Project-local skills and system prompts (`./.akern/`, honored only in trusted
directories) are not read: the kernel reads `AKERN_HOME` alone. Smallest
version: the front end passes `-S` a merged system prompt when the project is
trusted. Decide when a real project needs it.

### Jobs after a kernel restart

A job dies with its kernel; after `SIGKILL` of the kernel its `tool_pending`
stays open in the session.

| Option | Cost |
|---|---|
| nothing (today) | the open `tool_pending` stays open on resume |
| on open, close each open `tool_pending` with a `job_result` saying it was lost | a session scan at open; the kernel must find open pendings in the chain |
| jobs write straight to their files and a restarted kernel tails them, re-adopting survivors | file-based capture for every job, for an uncommon case |

Tools that must outlive the kernel (servers) can daemonize themselves under
any option.

### Orphans after the kernel is killed on macOS

There is no `PR_SET_PDEATHSIG`, so `SIGKILL` of the kernel can leave workers
running ([kernel.md: proc](kernel.md#proc)).

| Option | Cost |
|---|---|
| accept it (today) | orphans after a hard kill |
| a supervisor per worker that kills its group when a pipe from the kernel hits EOF | one more process per call |
| one helper watching the kernel with `kqueue` `EVFILT_PROC` | one process per kernel; macOS-only code |
