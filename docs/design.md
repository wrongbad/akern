# Design

Why akern is shaped the way it is. The contracts themselves are in the API
docs: [akern.md](akern.md) (the process), the comment atop each file in `akern/`
and the headers in `shared/` (its modules), [providers.md](providers.md) and [tools.md](tools.md).

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

- **Smallest kernel that is still a complete agent.** C99, POSIX only, no
  libraries, no threads. Readable in an afternoon.
- **Concurrency is core, not a feature.** Parallel tool calls, background jobs,
  steering at any moment, and sub-agents are designed in from the start. They are what stress an agent architecture, and retrofitting them
  is what breaks one.
- **Full pi parity is reachable without touching the kernel.** Every pi feature
  maps to a tool, a provider, a frontend or a command
  ([pi-parity.md](pi-parity.md)).
- **Extensions in any language.** A shell script is a first-class extension.
- **Contracts are stable; implementations are replaceable.** Including the
  kernel itself. Stability is the goal, not the current state: while the
  project is pre-alpha, any contract may break at any commit, with no
  migration.

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
   flight per session, since two requests from one history would fork the
   conversation. Everything the model launches runs alongside it. Parallelism
   across conversations is more kernels: a sub-agent is another `akern`
   on another session file.
4. **Nothing blocks the loop.** A permission prompt in a shim or a
   ten-minute build stalls only its own call.
5. **Every call has an answer.** Each `tool_use` gets exactly one
   `tool_result`: in the next message when it ends within its turn (a
   cancellation included), else later, when its job ends. A provider answers
   a call that is still running with a placeholder of its own, so the session
   records only what happened. Interruptions never leave the history invalid.
6. **One wire format.** UTF-8 JSON lines everywhere. If you can read one
   interface you can read them all.
7. **The file is the state, and the stream is the file.** A session is an
   append-only JSONL file holding everything the kernel said on stdout, in
   order, as said. There is no second account of a session to fall out of step with the
   first: a front end that reads stdout and one that tails the file see the
   same lines. Resume is opening the file; a fork is its first lines;
   compaction is a `reset` marker and a summary. A job's result is logged when
   it ends, and its output is in its own file meanwhile, so what ran and what
   still runs is in files. The inbox is the one exception: queued items are
   recorded (`queued`) but a resume does not queue them again.
8. **Slice, don't parse.** The kernel treats messages as opaque byte spans and
   never re-serializes them. Coalescing joins the strings of display deltas
   (`text_delta`, `tool_progress`), which no request is built from. What a provider emits is stored and sent back
   byte-for-byte, and new messages only ever go at the tail. This keeps prompt
   caches warm and provider-specific fields (thinking signatures, cache
   markers) intact for free.
9. **Discovery by `PATH` and one directory.** Put an `akern-*` tool on `PATH`
   and it can be used. Drop a provider in `~/.akern/providers/` and the next
   request can use it. The system prompt is a program's output: the kernel
   runs `~/.akern/system-prompt`, else `~/.akern/system/system-prompt`, once at startup, and
   what assembles the prompt (skills, `AGENTS.md`, your own text) is that
   program's business. No registry, no manifest, no reload command.
10. **Fail closed.** A broken permission gate must never mean "allowed": a shim
    that crashes runs nothing, and a sandbox that fails to start starts no kernel.
11. **If the model can do it, we don't ship it.** The system provides only
    what the model cannot build for itself. It can choose a shell, persist
    state between calls, look at a background job's output, edit files and
    write scripts, so none of that is our code. What it needs from us is what a
    process needs from an OS kernel, its core syscalls: the agent kernel gives
    it `shell` and `job`, over files and jobs (files are read and written
    with `cat` in the shell). One convenience is shipped as a tool too, `edit`, because nearly
    every coding turn uses it and models are trained on it. Each tool runs a
    program (`bash`, `akern-edit`) that `tools.json` names (`job` runs in the kernel), so
    none is built into the kernel. The one guard in `shell`
    is the output limit, which spills to a file and loses nothing; it has to be
    in the kernel, because once output is in the context it is too late.
12. **The command line is the interface, for the model too.** Every other
    capability is a program on `PATH` plus a skill that says how to call it
    (`akern -p` for a sub-agent, `gh`, a project's scripts). A capability added for people is automatically
    available to the model, and the reverse. No wrappers: a wrapper exists
    only because an API is missing, so fix the API instead.
13. **Every completion wakes the model.** A finished command or job is always
    delivered; no option suppresses or defers a completion to save tokens
    ([akern.md: delivery modes](akern.md#delivery-modes)). A silent exit, like a
    crashed dev server, can ruin a long unattended session. A person may still
    send their own message `deferred`.
14. **Content is a black box, but for five signatures.** akern parses no
    file format. It knows only the first bytes of the five formats every
    provider takes, PNG, JPEG, GIF, WebP and PDF, and a `shell` call whose stdout
    starts with one comes back as an image or document; everything else is
    text. The API decides what it accepts; a block the API rejects is replaced
    with a note in the session, so no later request sends it again.
15. **Tools don't know each other.** A tool's output, errors and usage text
    included, never names another tool or built-in, and an error states the
    problem, never a remedy. Advice, and how tools fit together, belongs in
    skills. Naming a sibling couples them; any program must be replaceable
    without another's text going stale.
16. **Docs mirror directories and executables.** One reference doc per
    directory or executable: [akern.md](akern.md) for the kernel
    process, the header comments for `akern/` and `shared/`, [providers.md](providers.md)
    for `providers/`, [tools.md](tools.md) for `tools/` and `skills/`. Every
    fact lives in one place; other docs link to it. A walkthrough such as
    [turn.md](turn.md) orders those facts and links to them.
17. **Providers keep no model tables.** A provider sends the model id as
    given and compiles in no list of models, limits or features: `--models`
    asks the API, and what a model refuses comes back as the API's error. A
    table goes stale with every release. Where the backend has no list API,
    its stable aliases (which track the latest model) are the list:
    `claude-code` lists `claude`'s aliases.

## Seams

| Seam | Contract |
|---|---|
| Front end: whatever drives the kernel | commands on stdin, events on stdout: [akern.md](akern.md) |
| Provider: request in, response out | [providers.md](providers.md) |
| Tools: `shell`, `job`, `edit`, and programs on `PATH` | [tools.md](tools.md) |

The front end's pipes are the kernel's only channels. Whoever starts a kernel
owns the conversation with it, and the pipes along the process tree are enough:
a person types into the terminal `akern-cli` runs in, a parent agent sends `job` input to its
sub-agent's stdin pipe. There is no control socket. What that gives up, on
purpose: attaching a second UI to an agent already running elsewhere (run it in
tmux; a separate relay program could add it later without kernel changes), and
programs other than the front end appending to an owned session (an open item;
see [roadmap.md](roadmap.md#appending-to-an-owned-session)).

What the kernel knows: commands and lines, a session's messages after its
last `reset`, the `tool_use` blocks of an assistant message, which block a
provider says the API rejected, child processes, files and a job's stdin, and the order of
deliveries. What it does not know: HTTP, TLS, any
provider's API, any program's semantics, markdown, terminals, `AGENTS.md`,
skills, compaction, permissions, config files.

## One stream, five actors

Inside, the kernel has the shape it shows outside: one stream of lines, with
programs reacting to it. Every line goes through one queue, the bus, and is
handed out in order to stdout, to the session file and to five actors, one
file each, as in a shell pipeline of filters. An
actor does something only when a line reaches it, and talks to the others only
by putting lines on the bus; no module calls into another's state. Orders
between actors are *told* lines: they go to the actors only, never to stdout
or the file, so the protocol does not change when the wiring does.

| Actor | Reacts to | Puts on the bus |
|---|---|---|
| loop | `exit`: stops reading stdin and every process, and returns once none is left | for a signal: `interrupt` and an abort, or `exit` |
| input | stdin; `status`, answered in its place among the commands | told: each command, `eof` |
| tools | a reply that asks for tools: runs them; the message that answers the turn: the rest become jobs; `abort`: kills; `due`: a wait ran out | `tool_start`, `tool_progress`, `block`; told: `released`, `due` (a timer) |
| model | `continue`: starts a turn; `preempt`, `abort`: stops the provider and seals the partial reply; its own `replace`: retries | `turn_start`, deltas, the reply, `replace`, `turn_end` |
| inbox | send, abort and append, the turn's lines, `block`, `released`, `eof`, `exit`, `close` | user messages, `run_start`, `run_end`, `pending`, `preempt`, `abort`, `returned`, held entries, replies, `settled`; told: `continue`, `exit` |

The inbox is the sequencer: it alone decides what happens next, when queued
items and call results go into the conversation, when the model talks
(`continue`), what an abort stops, and when the kernel is done (`exit`). The
others are servants: the model starts a turn because it is told to, not
because it guessed from a user message. A line put while lines are handed out
waits its turn, so no reaction runs inside another, and each actor's state
changes at once with what it says, so it never says it twice. Processes, timers
and signals are just more sources of lines. The main loop is the whole
kernel:

```c
for (;;) {
    bus_drain(bus);      /* each line: stdout, session, then each actor */
    if (stopping && no processes)
        return exit_code;
    wait_for_io(loop);   /* stdin, processes, signals, timers: lines onto the bus */
}
```

Each file opens with what it owns; `akern.h` is what they share, and lists
the told lines:

| File | |
|---|---|
| `akern.h` | the shared types and the bus, loop and process calls |
| `main.c` | arguments, wiring, the first lines |
| `bus.c` | the queue, timers, stdout, the session file |
| `loop.c` | poll, signals, timers' clock, the exit line |
| `process.c` | child processes: providers and tool programs |
| `input.c`, `tools.c`, `model.c`, `inbox.c` | the actors |

Why: the order of the conversation is the order of the stream. A message that
cuts into a reply lands in the file after the reply began, and the reply sits
in the context where it began, so neither the file nor the request ever needs
rewriting. And with no second path to the file, stdout and the session cannot
disagree.

## Programs and home

One rule decides where a thing lives: **what people and the model run is on
`PATH`; what the kernel starts is in `AKERN_HOME`** (default `~/.akern`; the
variable exists for development and tests). Layout:
[akern.md §5](akern.md#home), [tools.md §3](tools.md#install).

Providers are kept off `PATH` on purpose. One is never run by hand (it reads a
request on stdin), and it is trusted with a lot: it sees the whole conversation
and holds an API key. Installing a program must never change which model
backends exist. `ls ~/.akern/providers ~/.akern/system/providers` answers
"which models can I use".

A **package** is some tools plus, optionally, providers and skills: put the
tools on `PATH` and copy the rest into `AKERN_HOME`. `make install` does exactly
that for this repository.

## Deliberately outside the kernel

| Concern | Where it lives |
|---|---|
| Talking to a model API, auth, retries, cache breakpoints | the provider |
| `AGENTS.md`, skills, system prompt assembly | the system-prompt program, run once at startup: `$AKERN_HOME/system-prompt`, else the shipped `$AKERN_HOME/system/system-prompt`; or a fixed file with `-S`; a wrapping provider per request |
| Prompt templates, `!cmd`, `@file` expansion | the frontend |
| How pending calls and job results are worded, or whether they need words at all | the provider |
| Permission gates, path protection, sandboxing | an OS sandbox around the whole kernel, or a shim on `PATH` |
| Which calls go to the background | the model (`"wait": false` on the call) |
| Output redaction, summarizing | a shim on `PATH` (the size cap is `shell`'s `output_limit`) |
| Compaction, fork, export, session listing | commands (`akern-*`) |
| Sub-agents | another `akern -p`, or any ACP agent through `akern-agent`, run through `shell`; the kernel only counts down `AKERN_RE_LIMIT` |
| Job control for the model | the provider's placeholder, naming the job and its file; `tail` through `shell`; `job` for input, end of input, signals and waiting; or end the turn |
| Steering | a person types into the terminal; a parent sends a line to its sub-agent with `job` |
| Shells, file editing, state between calls | the model, using whatever is installed |
| Settings files, model aliases | the `akern-cli` front end and frontends |
| Rendering, key bindings, themes, editors | frontends; `akern-cli`'s display is `$AKERN_HOME/render.jq`, else `$AKERN_HOME/system/render.jq` |
| MCP, plan mode, todos | commands, skills, providers and frontends |

## No interception points

The kernel runs no hooks: nothing sits between it and a request, a tool call or
a result. Interception is built in user space, at the seams it already has.

| Seam | How |
|---|---|
| Provider | wrap or replace `$AKERN_HOME/providers/<name>`: change the request on stdin, `exec` the real provider, or filter its output |
| Tools | every call runs the program its tool's `program` in `tools.json` names (`akern-shell -c COMMAND`, `akern-edit PATH …`), looked up on `PATH`, and the shell looks up each program it runs on `PATH` in turn, so a shim earlier on `PATH` wraps a whole tool or any one program: check, rewrite, log, redact. Your own `tools.json` can also point a tool at another program ([tools.md](tools.md#1-built-in-tools)) |
| Front end | a program that writes the kernel's stdin commands and reads its stdout lines |

| Pi hook point | Here |
|---|---|
| `input` | the front end rewrites a prompt before it sends it |
| `request` | a wrapping provider; the system prompt is the system-prompt program's output at each start, or `-S` |
| `tool_call` | a shim on `PATH`; an OS sandbox around the whole kernel |
| `tool_result` | a shim on `PATH` that filters its program's output |
| `turn_end` | the front end, on the `turn_end` line: logging, checkpoints |
| `idle` | the front end, on `run_end`: `send` to continue or retry |

A wrapper named as the shell tool's `program` sees every `shell` call's
command (a shim of `bash` on `PATH` would too, but also every other bash on
the machine), but not what the commands go on to do (a script's file access,
a program run by absolute path). Confine the whole kernel in an OS
sandbox (a container, `bwrap`, `sandbox-exec`), as pi does, for that.

## No named pipes

A job's stdin, a sub-agent's included, is an anonymous pipe whose write end
only its parent kernel holds. Input reaches it through the parent (`job`, or a
front end's line on the kernel's stdin), never through a FIFO or socket path
that other processes open. A path would bring two problems the tree does not
have:

- **Cleanup.** A FIFO is a file. Someone has to create it, pick a name that
  does not collide, and remove it when the job ends, including after a kernel
  killed with `SIGKILL` that never got to clean up. A stale one misleads the
  next process that finds it. An anonymous pipe is gone when the last process
  holding it is, and "kill everything it started" already covers that.
- **More than one writer.** Anyone who can open the path can write to it.
  Writes larger than `PIPE_BUF` are not atomic, so two writers can interleave
  in the middle of a line, and avoiding that means locks, framing and handling
  for half-written input. The reader also sees EOF whenever the last writer
  closes, so the job's input ends between two writers unless something keeps
  the FIFO open.

In the tree, each child has exactly one writer: its parent. Everything sent to
a child goes into one queue in the parent, in order, so there is nothing to
lock and nothing to remove. Anyone else who wants to reach a child, such as a
person, a sibling or a grandparent, goes through its parent. The cost is that
you cannot steer a job from another terminal on the side; run the kernel
under tmux, or send it through the front end that owns the kernel.

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
  shim that must ask the user something uses `AKERN_ASK`
  ([tools.md](tools.md#akern_ask)), and the frontend decides what that looks
  like.
- **The HTTP providers have a dependency** (libcurl). The kernel stays
  dependency-free; the HTTP stack is `providers/http.c`, linked only into
  the providers that call an API over HTTP.

## Security posture

Same stance as pi: no built-in permission prompts. Out of the box the model can
run any tool with your privileges. Safety is composed:

- gate a whole tool by naming a wrapper as its `program` in your
  `tools.json` (or with a shim of `akern-edit` on `PATH`),
  and individual programs with a shim of their own on `PATH`,
- restrict what the model can run with a restricted `PATH`,
- or run the whole thing inside a container, VM or OS sandbox. One process
  tree with one working directory is easy to confine, and only this covers
  what a program does once it runs. It needs no
  kernel change: the front end starts `akern` inside the container
  (`docker run -i …`), and the pipes cross the boundary like any other.
  API keys then stay outside it, in a key proxy that gives each container a
  token of its own ([sandbox.md](sandbox.md), a design not yet built).

Only the process that started a kernel can drive it: there is no listening
endpoint. A job's stdin pipe is anonymous: only the kernel holds its write
end, and the model reaches it only through `job`.

The kernel's own obligations are narrow: never run project code that the user
did not trust, kill everything it started, and
never emit a line that is not valid JSON.
