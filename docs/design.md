# 01 — Design

## What this is

An agent harness is a loop: send the conversation to a model, run the tools it
asks for, append the results, repeat. Pi showed that a harness can ship a small
core and let extensions supply everything else. This project takes the same idea
one step further down: the core is a C program with no dependencies, and an
extension is not a module loaded into it but a **process next to it**.

The unit of extension is the executable. The interface is JSON lines on stdin
and stdout. The state is a file. And the core never waits: every child is a
worker on one event loop, so parallel tool calls, background jobs, sub-agents
and messages that interrupt a turn are part of the kernel from the start.

## Goals

- **Smallest kernel that is still a complete agent.** Budget: 3,000 lines of
  C99, POSIX only, no libraries, no threads. Readable in an afternoon.
- **Concurrency is core, not a feature.** Parallel tool calls, background jobs,
  steering at any moment, and sub-agents are designed in from the first
  milestone. They are what stress an agent architecture, and retrofitting them
  is what breaks one.
- **Full pi parity is reachable without touching the kernel.** Every pi feature
  maps to a tool, a provider, a frontend or a command
  (see [pi-parity.md](pi-parity.md)).
- **Extensions in any language.** A shell script is a first-class extension.
- **Contracts are stable; implementations are replaceable.** Including the
  kernel itself.

## Non-goals

- In-process plugins: no `dlopen`, no embedded Lua/JS, no ABI.
- Threads. Concurrency comes from processes and `poll(2)`.
- A built-in TUI, permission system, MCP client, plan mode or package manager.
  All are userland.
- Windows. POSIX only (macOS, Linux, BSD; WSL works).
- Hiding the conversation format. Sessions are plain JSONL you can `grep`.

## Principles

1. **Mechanism, not policy.** The kernel moves bytes between processes and
   keeps the log. It decides nothing about what is allowed, what is shown or
   what goes in the prompt.
2. **Everything is a process.** Provider, tools, jobs, sub-agents and
   frontends are executables. A crash takes down one call, never the session.
3. **The conversation is serial; the work is concurrent.** One model request in
   flight per session. Everything the model launches runs alongside it.
   ([concurrency.md](concurrency.md))
4. **Nothing blocks the loop.** A permission prompt in a shim or a
   ten-minute build stalls only its own call.
5. **Every call has an answer.** Each `tool_use` is answered before the next
   request: by its result, a cancellation, or a `tool_pending` that a
   `job_result` closes later. Interruptions never leave the history invalid.
6. **One wire format.** UTF-8 JSON lines everywhere. If you can read one
   interface you can read them all.
7. **The file is the state.** A session is an append-only JSONL file. Model,
   parameters and queues are the only state the kernel holds outside it.
8. **Slice, don't parse.** The kernel treats messages as opaque byte spans and
   never re-serializes them. What a provider emits is stored and sent back
   byte-for-byte. This keeps prompt caches warm and provider-specific fields
   (thinking signatures, cache markers) intact for free.
9. **Discovery by `PATH` and one directory.** Put an `akern-*` tool on `PATH`
   and it can be used. Drop a provider in `~/.akern/providers/` and the next
   request can use it. The system prompt is a file, `~/.akern/system.md`. No
   registry, no manifest, no reload command.
10. **Fail closed.** A broken permission gate must never mean "allowed": a shim
    that crashes runs nothing, and a sandbox that fails to start starts no kernel.
11. **If the model can do it, we don't ship it.** The system provides only
    what the model cannot build for itself. It can choose a shell, persist
    state between calls, look at a background job's output, edit files and write scripts, so
    none of that is our code. What it needs from us is what a process needs
    from an OS kernel, its core syscalls: the agent kernel gives it `exec`,
    `read` and `write`, over files and jobs. Everything else is a program plus
    a `SKILL.md`.
12. **The command line is the interface, for the model too.** Every other
    capability is a program on `PATH` plus a skill that says how to call it
    (`akern-loop -p` for a sub-agent, `akern-edit` to change a file,
    `gh`, a project's scripts). A capability added for people is automatically
    available to the model, and the reverse. No wrappers: a wrapper exists
    only because an API is missing, so fix the API instead.

### The model's toolset: three primitives

Like an OS kernel's core syscalls, the agent kernel gives the model `exec`,
`read` and `write`, over files and jobs; everything else is a program plus a
`SKILL.md`. Their definitions are compiled in, and they are what the model is
told, nothing more:

```json
{"name":"exec","description":"Run a program. … The result is JSON: {stdout, stderr, exit} …",
 "input_schema":{"type":"object","properties":{
   "argv":{"type":"array","items":{"type":"string"}},"stdin":{"type":"string"},"stdin_open":{"type":"boolean"},
   "wait":{"type":"boolean"},"stdout_type":{"type":"string"},
   "stdout_limit":{"type":"integer"},"stderr_limit":{"type":"integer"}}}}
{"name":"read","description":"Read a file. …","input_schema":{"type":"object","properties":{
   "path":{"type":"string"},"offset":{"type":"integer"},"limit":{"type":"integer"},"type":{"type":"string"}}}}
{"name":"write","description":"Write text to a file, or to the stdin of a running program started with stdin_open …",
 "input_schema":{"type":"object","properties":{
   "path":{"type":"string"},"job":{"type":"string"},"content":{"type":"string"},
   "append":{"type":"boolean"},"close":{"type":"boolean"}}}}
```

The kernel runs `argv` directly (`execvp`, `PATH` lookup, stdin from the call's `stdin` text or empty) and
returns `{"stdout": …, "stderr": …, "exit": …}`, losslessly. Each stream is
kept in the result up to `stdout_limit` or `stderr_limit` bytes (default
100000 each; the model can change either). A longer stream is saved whole to a file, and the result names the
file and the total size, so nothing is lost and one `cat` of a huge log cannot
flood the context. This is the one guard that protects every program, and it
has to be in the kernel: once output is in the context it is too late. With
`stdout_type`, a MIME type, stdout comes back as an image or document block
instead, so any program that generates an image (a screenshot tool, a chart
renderer) lets the model see it. There is no shell unless the model runs one: `["sh","-c","make 2>&1 | tail"]`, or `bash`,
or `zsh`, or whatever the machine has. A call waits for its program unless
the model passes `"wait": false` (a server, a watcher, a sub-agent), or a
message arrives first: then the call is answered pending and the program
continues as a job. The model knows what it need not wait for; the kernel has
no default cutoff. The tool is an invented input format,
but the smallest possible one: it is `execve`.

`read` and `write` run inside the kernel and finish at once. `read` returns a
file's exact text, 2000 lines at a time from `offset` (at most 100000 bytes,
with a note saying where to read on), or, given a MIME `type`, the file as an
image or document block. The kernel keeps no list of formats; the provider
knows which ones its API accepts. `write` replaces a file atomically or appends to
it, and the content never passes through a shell. Neither runs a program, so no
shim sees them; an OS sandbox around the kernel confines them.

A pending call's output goes to `<session>.d/calls/<call id>.out` and `.err`,
and its `tool_pending` names both and the pid, so checking on a job is just
`tail` and stopping one is `kill -TERM -- -PID` (each call has its own process
group). With `stdin_open`, the kernel keeps the program's stdin pipe open, so
the model can keep talking to a REPL or a sub-agent with
`write {"job":"<call id>","content":"…"}`. There is no
side channel between a program and the kernel. Permission policy is a shim
earlier on `PATH` that reads its `argv`: `argv[0]` is exact, but whatever the
model passes to `sh -c` is still an arbitrary script. Sandboxing is the whole
kernel inside `bwrap`, `sandbox-exec` or a container
([No interception points](#no-interception-points)).

### Extending: a command and a skill

There is no plugin API and no tool registry. To teach the model something new,
put a program on `PATH` and write `$AKERN_HOME/skills/<name>/SKILL.md`
saying when and how to call it. `make install` lists every shipped skill's
one-line description in `$AKERN_HOME/system.md`, the system prompt when there
is no `-S`; add a line there for a skill of your own. The model reads the full
file when it needs it
([protocols.md §8](protocols.md#8-commands-and-skills)). This covers
everything a JSON tool would: MCP servers through a bridge command, APIs
through their CLIs, project scripts as they are.

## Architecture

```
  frontend ── commands ──► stdin  ┌────────────────────────────┐ stdout ──► events ──► frontend
  (akern, tui, editor, script,    │         akern-loop         │
   parent agent's pipe)           │                            │
                                  │          inbox ─► driver   │
                                  │            ▲        │      │
                                  │            │   workers     │
                                  └───────┬────┴──┬──────────┬─┘
                                          │       │          │ sole writer
                                     providers/ exec         session.jsonl
                                       NAME     argv           + session.jsonl.d/calls/
```

A provider has the kernel's own shape: spawn an executable, write JSON to its
stdin, read JSON lines from its stdout. Beside it are the model's built-in
tools, of which `exec` runs any program, and the frontend that drives it all:

| Seam | What it is | Found at | Contract |
|---|---|---|---|
| **Provider** | Turns a normalized request into a model response | `$AKERN_HOME/providers/<name>` | request in, event stream out, one final `result` line |
| **exec, read, write** | The model's built-in tools | built in; programs on `PATH` | argv in, `{stdout, stderr, exit}` back; a path or job in, text, media or a confirmation back |
| **Frontend** | Whatever drives the kernel | owns stdin/stdout | commands in, events out |

The frontend's pipes are the kernel's only channels. Whoever starts a kernel
owns the conversation with it, and the pipes along the process tree are enough:
a person types into the terminal `akern` runs in, a parent agent `write`s to its
sub-agent's stdin pipe. There is no control socket (an earlier draft had one).
What that gives up, on purpose: attaching a second UI to an agent that is
already running elsewhere (run it in tmux; a separate relay program could add
it later without kernel changes), and programs other than the frontend appending
to an owned session (an open item; see [roadmap.md](roadmap.md#settled-since-the-first-draft)).

A fourth thing, the **command**, is not a kernel seam at all: any `akern-foo` on
`PATH` is one, run directly by people, scripts and the model alike. There is
no dispatcher; `PATH` already is one. Session tools
(`akern-fork`, `akern-compact`, `akern-sessions`), package management and login
flows live here. A slash command in an interactive frontend is the same lookup:
`/compact` runs `akern-compact` against the current session.

## The kernel's whole job

The kernel is an event loop driving one state machine, the **driver**, over a
pool of **workers**:

```
driver:
  IDLE     inbox has an item due?                → append it, spawn provider, STREAM
  STREAM   forward deltas; on result: append it
           tool_use blocks?                      → start one call per block, WAITING
           otherwise, now/turn/idle items due?   → append them, spawn provider, STREAM
           otherwise                             → run_end, IDLE
  WAITING  waited calls done and unwaited ones started, or a now/turn item due?
                                                 → append [result or tool_pending per call
                                                    + due inbox items], spawn provider, STREAM

  a `now` item interrupts a streaming response (concurrency.md#preemption)

each call (all in parallel):
  exec: a --max-processes slot → process → done;  read, write: done at once
       └── unfinished when the turn is answered: pending, a job; its job_result returns via the inbox
```

What the kernel knows:

- how to read commands and emit events,
- how to walk a parent chain in the session file,
- how to find `tool_use` blocks in an assistant message,
- how to supervise many child processes,
- how to read and write a file or a job's stdin,
- how to order deliveries into the conversation.

What it does not know: HTTP, TLS, any provider's API, any program's semantics,
markdown, terminals, `AGENTS.md`, skills, compaction, permissions, config files.

## Programs and home

One rule decides where a thing lives: **what people and the model run is on
`PATH`; what the kernel starts is in `AKERN_HOME`.**

On `PATH`, like any other Unix program:

```
akern                       the front end for people: an optional first prompt, then each
                            line typed (steering while it works), through akern-loop and render.jq
akern-loop                  the kernel; with -p it prints only final answers and exits when done
                            (sub-agents, scripts)
akern-edit                  exact-match replacement in a file, for the model
```

In `AKERN_HOME`, which defaults to `~/.akern`:

```
$AKERN_HOME/
  providers/<name>          providers; `-m anthropic/…` starts providers/anthropic
  system.md                 the system prompt without -S; `make install` lists the skills in it
  skills/<name>/SKILL.md    how to call a tool
  render.jq                 how `akern "prompt"` shows the event stream; edit to taste
```

The variable exists for development and tests (`akern.sh` points it at the
build directory); normal use never sets it.

Providers are kept off `PATH` on purpose. One is never run by hand (it reads a
request on stdin), and it is trusted with a lot: it sees the whole conversation
and holds an API key. Installing a program must never change which model
backends exist. `ls ~/.akern/providers` answers "which models can I use".

A **package** is some tools plus, optionally, providers and skills: put the
tools on `PATH` and copy the rest into `AKERN_HOME`. `make install` does exactly
that for this repository. It replaces the providers it ships and leaves skills,
`system.md` and a `render.jq` that are already in `~/.akern` alone.

## Sessions: the file is the state

A session is an append-only JSONL file. Every entry has an `id` and a `parent`,
so the file is a tree. Two rules define everything:

1. **The head is the last entry in the file.**
2. **The context is the chain of `message` entries from the head back to a root.**

Consequences:

- **Resume** is opening the file.
- **Branch in place** (pi's `/tree`) is appending one marker entry whose parent
  is the entry you want to return to. It becomes the head; the next message
  hangs off it.
- **Fork** (pi's `/fork`) is copying a chain into a new file.
- **Compaction is a branch.** Start a new root whose first message is the
  summary, re-append the recent messages to keep, and continue from there. The
  kernel has no compaction concept; it only ever walks to a root. The old chain
  stays in the file, untouched.
- **Extension state** is any entry whose `type` is not `message`. The kernel
  skips it when building context and preserves it in the chain.
- **Background work is logged.** A job's start and end are its `tool_pending`
  and `job_result` blocks, and its output lives in `<session>.d/calls/`.
  Listing what ran, and what is still running, means reading files.

**One writer.** While a kernel has a session open, it is the only process that
appends to it. Its frontend can send an `append` command on stdin, and the
kernel serializes those appends with its own; nobody else writes to it. When no kernel holds the
session, writers append directly under `flock`. Structural appends (a new root, a
`branch`) sent while calls are in flight wait for the next turn boundary, so a
tool result can never land on a chain that no longer holds its call.

## Deliberately outside the kernel

| Concern | Where it lives |
|---|---|
| Talking to a model API, auth, retries, cache breakpoints | the provider |
| `AGENTS.md`, skills, system prompt assembly | `$AKERN_HOME/system.md` or `-S`, written ahead of time; a wrapping provider per request |
| Prompt templates, `!cmd`, `@file` expansion | the frontend |
| How pending calls and job results are worded | the provider |
| Permission gates, path protection, sandboxing | an OS sandbox around the whole kernel, or a shim on `PATH` |
| Which calls go to the background | the model (`"wait": false` on the call) |
| Output redaction, summarizing | a shim on `PATH` (the size cap is the kernel's `stdout_limit`, which spills to a file and loses nothing) |
| Compaction, fork, tree, export, session listing | commands (`akern-*`) |
| Sub-agents | another `akern-loop -p`, run through `exec` like any program (`wait: false`, `stdin_open: true`, `-s` naming the child's session); the kernel only counts down `AKERN_RECURSION_LIMIT` |
| Job control for the model | the `tool_pending` (pid, output files, whether stdin is open), worded by the provider; then `tail` and `kill -TERM -- -PID` through `exec`, and `write {"job":"<call id>"}` for input; to wait, end the turn |
| Steering | a person types into the terminal (a plain line arrives at the next turn boundary); a parent `write`s a line to its sub-agent's job |
| Shells, file editing, state between calls | the model, using whatever is installed |
| Settings files, model aliases | the `akern` front end and frontends |
| Rendering, key bindings, themes, editors | frontends; one-shot output is `$AKERN_HOME/render.jq` |
| MCP, plan mode, todos | commands, skills, providers and frontends |

## No interception points

The kernel runs no hooks: nothing sits between it and a request, a tool call or
a result. Interception is built in user space, at three seams it already has.

| Seam | How |
|---|---|
| Provider | wrap or replace `$AKERN_HOME/providers/<name>`: change the request on stdin, `exec` the real provider, or filter its output |
| Tools | `exec` looks `argv[0]` up on `PATH`, so a shim earlier on `PATH` wraps any program: check, rewrite, log, redact |
| Front end | a program that writes the kernel's stdin commands and reads its stdout events |

| Former hook point | Now |
|---|---|
| `input` | the front end rewrites a prompt before it sends it |
| `request` | a wrapping provider; a fixed system prompt is `$AKERN_HOME/system.md` |
| `tool_call` | a shim on `PATH`; an OS sandbox around the whole kernel |
| `tool_result` | a shim on `PATH` that filters its program's output |
| `turn_end` | the front end, on the `turn_end` event: logging, checkpoints |
| `idle` | the front end, on `run_end`: `send` to continue or retry |

Not covered: a permission prompt or output redaction per call inside one
kernel, for `read` and `write` (which run in the kernel) and for programs no
shim wraps (an absolute path, a command inside `sh -c`). Confine the whole
kernel in an OS sandbox (a container, `bwrap`, `sandbox-exec`), as pi does, or
put the policy in an `exec` shim on `PATH`.

## What this design costs

Stated plainly so they can be argued with:

- **The kernel is an event loop, not a script.** Supervising many children,
  waking turns and delivery ordering are about a third of the kernel. The
  alternative was a smaller kernel that would have needed rewriting the first
  time someone wanted to steer a running agent.
- **A process per call, and per shim.** With parallel calls, one turn can mean
  dozens of `fork`/`exec` calls. That costs milliseconds against model calls
  that take seconds, but it adds up with slow-starting runtimes. The
  mitigation is on the extension's side: a thin executable that talks to its
  own long-lived daemon.
- **Shims must be reentrant.** Parallel calls run the same shim at the same
  time.
- **Extensions cannot share memory with the kernel.** Anything stateful keeps
  its state in a file or a daemon. This is the point, but it makes some pi
  extensions (live TUI widgets driven from extension code) a frontend concern
  rather than a kernel one.
- **Rich UI needs a frontend that cooperates.** The kernel will never draw. A
  shim that must ask the user something uses the `AKERN_ASK` helper
  ([protocols.md](protocols.md#asking-the-user)), and the frontend
  decides what that looks like.
- **The reference provider is a second binary with a dependency** (libcurl).
  The kernel stays dependency-free; the HTTP stack is isolated in one
  replaceable program.

## Security posture

Same stance as pi: no built-in permission prompts. Out of the box the model can
run any tool with your privileges. Safety is composed:

- gate individual programs with a shim on `PATH`,
- restrict what the model can run with a restricted `PATH`,
- or run the whole thing inside a container, VM or OS sandbox. One process
  tree with one working directory is easy to confine, and only this covers
  `read` and `write`.

Only the process that started a kernel can drive it: there is no listening
endpoint. A `stdin_open` pipe is anonymous: only the kernel holds its write
end, and the model reaches it only through `write`.

The kernel's own obligations are narrow: never run project code that the user
did not trust, kill everything it started, and
never emit a line that is not valid JSON.
