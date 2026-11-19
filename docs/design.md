# Design

Why akern is shaped the way it is. The contracts themselves are in the API
docs: [akern.md](akern.md) (the process), [kernel.md](kernel.md)
(its modules), [providers.md](providers.md) and [tools.md](tools.md).

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
   flight per session, since two requests from one head would fork the
   conversation. Everything the model launches runs alongside it. Parallelism
   across conversations is more kernels: a sub-agent is another `akern`
   on another session file.
4. **Nothing blocks the loop.** A permission prompt in a shim or a
   ten-minute build stalls only its own call.
5. **Every call has an answer.** Each `tool_use` is answered before the next
   request: by its result, a cancellation, or a `tool_pending` that a
   `job_result` closes later. Interruptions never leave the history invalid.
6. **One wire format.** UTF-8 JSON lines everywhere. If you can read one
   interface you can read them all.
7. **The file is the state.** A session is an append-only JSONL file. Model,
   parameters and queues are the only state the kernel holds outside it.
   Resume is opening the file; a branch is one appended marker; compaction is
   a new root. Background work is logged as `tool_pending` and `job_result`
   blocks, so what ran and what still runs is in files.
8. **Slice, don't parse.** The kernel treats messages as opaque byte spans and
   never re-serializes them. What a provider emits is stored and sent back
   byte-for-byte, and new messages only ever go at the tail. This keeps prompt
   caches warm and provider-specific fields (thinking signatures, cache
   markers) intact for free.
9. **Discovery by `PATH` and one directory.** Put an `akern-*` tool on `PATH`
   and it can be used. Drop a provider in `~/.akern/providers/` and the next
   request can use it. The system prompt is a file, `~/.akern/system.md`, else
   `~/.akern/system/system.md`. No registry, no manifest, no reload command.
10. **Fail closed.** A broken permission gate must never mean "allowed": a shim
    that crashes runs nothing, and a sandbox that fails to start starts no kernel.
11. **If the model can do it, we don't ship it.** The system provides only
    what the model cannot build for itself. It can choose a shell, persist
    state between calls, look at a background job's output, edit files and
    write scripts, so none of that is our code. What it needs from us is what a
    process needs from an OS kernel, its core syscalls: the agent kernel gives
    it `exec`, `read` and `write`, over files and jobs. The one guard in `exec`
    is the output limit, which spills to a file and loses nothing; it has to be
    in the kernel, because once output is in the context it is too late.
12. **The command line is the interface, for the model too.** Every other
    capability is a program on `PATH` plus a skill that says how to call it
    (`akern -p` for a sub-agent, `akern-edit` to change a file,
    `gh`, a project's scripts). A capability added for people is automatically
    available to the model, and the reverse. No wrappers: a wrapper exists
    only because an API is missing, so fix the API instead.
13. **Every completion wakes the model.** A finished command or job is always
    delivered; no option suppresses or defers a completion to save tokens
    ([kernel.md: inbox](kernel.md#delivery-modes)). A silent exit, like a
    crashed dev server, can ruin a long unattended session. A person may still
    send their own message `deferred`.
14. **Content is a black box.** akern's code does not know what a JPEG, a
    WebP or a PDF is: no format detection, no header parsing, no list of file
    types. It moves the bytes as given; the model names a MIME type (`read`'s
    `type`, `exec`'s `stdout_type`) when it wants them shown as an image or
    document; the API decides what it accepts; a block the API rejects is
    replaced with a note in the session, so no later request sends it again.
    A fixed list goes stale, and the model judges file types better.
15. **Tools don't know each other.** A tool's output, errors and usage text
    included, never names another tool or built-in, and an error states the
    problem, never a remedy. Advice, and how tools fit together, belongs in
    skills. Naming a sibling couples them; any program must be replaceable
    without another's text going stale.
16. **Docs mirror directories and executables.** One reference doc per
    directory or executable: [akern.md](akern.md) for the kernel
    process, [kernel.md](kernel.md) for `src/`, [providers.md](providers.md)
    for `providers/`, [tools.md](tools.md) for `tools/` and `skills/`. Every
    fact lives in one place; other docs link to it.
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
| Tools: `exec`, `read`, `write`, and programs on `PATH` | [tools.md](tools.md) |

The front end's pipes are the kernel's only channels. Whoever starts a kernel
owns the conversation with it, and the pipes along the process tree are enough:
a person types into the terminal `akern-cli` runs in, a parent agent `write`s to its
sub-agent's stdin pipe. There is no control socket. What that gives up, on
purpose: attaching a second UI to an agent already running elsewhere (run it in
tmux; a separate relay program could add it later without kernel changes), and
programs other than the front end appending to an owned session (an open item;
see [roadmap.md](roadmap.md#appending-to-an-owned-session)).

What the kernel knows: commands and events, a session's parent chain, the
`tool_use` blocks of an assistant message, which block a provider says the API
rejected, child processes, files and a job's stdin, and the order of
deliveries. What it does not know: HTTP, TLS, any
provider's API, any program's semantics, markdown, terminals, `AGENTS.md`,
skills, compaction, permissions, config files.

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
| `AGENTS.md`, skills, system prompt assembly | `$AKERN_HOME/system.md` or `-S`, written ahead of time; a wrapping provider per request |
| Prompt templates, `!cmd`, `@file` expansion | the frontend |
| How pending calls and job results are worded | the provider |
| Permission gates, path protection, sandboxing | an OS sandbox around the whole kernel, or a shim on `PATH` |
| Which calls go to the background | the model (`"wait": false` on the call) |
| Output redaction, summarizing | a shim on `PATH` (the size cap is `exec`'s `stdout_limit`) |
| Compaction, fork, tree, export, session listing | commands (`akern-*`) |
| Sub-agents | another `akern -p`, run through `exec`; the kernel only counts down `AKERN_RECURSION_LIMIT` |
| Job control for the model | the `tool_pending`'s pid and files, worded by the provider; `tail` and `kill` through `exec`; `write {job}` for input; to wait, end the turn |
| Steering | a person types into the terminal; a parent `write`s a line to its sub-agent's job |
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
| Tools | `exec` looks `argv[0]` up on `PATH`, so a shim earlier on `PATH` wraps any program: check, rewrite, log, redact |
| Front end | a program that writes the kernel's stdin commands and reads its stdout events |

| Pi hook point | Here |
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
  shim that must ask the user something uses `AKERN_ASK`
  ([tools.md](tools.md#akern_ask)), and the frontend decides what that looks
  like.
- **The HTTP providers have a dependency** (libcurl). The kernel stays
  dependency-free; the HTTP stack is `providers/lib/http.c`, linked only into
  the providers that call an API over HTTP.

## Security posture

Same stance as pi: no built-in permission prompts. Out of the box the model can
run any tool with your privileges. Safety is composed:

- gate individual programs with a shim on `PATH`,
- restrict what the model can run with a restricted `PATH`,
- or run the whole thing inside a container, VM or OS sandbox. One process
  tree with one working directory is easy to confine, and only this covers
  `read` and `write`, which run in the kernel and pass no shim. It needs no
  kernel change: the front end starts `akern` inside the container
  (`docker run -i …`), and the pipes cross the boundary like any other.

Only the process that started a kernel can drive it: there is no listening
endpoint. A `stdin_open` pipe is anonymous: only the kernel holds its write
end, and the model reaches it only through `write`.

The kernel's own obligations are narrow: never run project code that the user
did not trust, kill everything it started, and
never emit a line that is not valid JSON.
