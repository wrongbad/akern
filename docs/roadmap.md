# 06 — Roadmap and open questions

## Milestones

Each milestone ends in something runnable and has one exit test. The order is
chosen so that the **concurrent architecture exists from M1**: no milestone
builds a single-child shortcut that a later one has to tear out.

### M0 — Contracts and test harness

- `json.c` with unit tests and a fuzz target.
- `providers/mock.sh` with pauses and gates; `tests/gate`; the invariant checker (`tests/invariants.py`)
  (the invariant checker).
- [protocols.md](protocols.md) reviewed against hand-written transcripts
  for each timeline in [concurrency.md](concurrency.md#timelines).

**Exit:** the invariant checker (`tests/invariants.py`) accepts the hand-written transcripts and rejects
deliberately broken ones (an unanswered `tool_use`, two open streams).

### M1 — Event-loop skeleton

- Worker table, poll loop, timers, self-pipe signals, backpressure.
- Driver states `IDLE → STREAM → WAITING`, plus `ABORTING` for preemption.
- Session ownership, index, append; control stream on stdin;
  `send` with `turn`/`idle`, `abort`, `set`, `status`.
- No tools yet.

**Exit:** a mock conversation over stdin, a plain line steering it at a turn
boundary, `abort` while streaming, all checked by the invariant checker (`tests/invariants.py`).

### M2 — Parallel tools

- Parallel calls, `--max-processes`, process groups, kill escalation.
- The built-in `exec`, `read` and `write` tools; skills listed in `system.md`.
- `providers/anthropic` (libcurl, streaming, aborted-result contract).
- The `akern` front end and `render.jq`.

**Exit:** `akern "add a hello-world test and run it"` completes a real task in
which the model makes parallel calls; gate scenarios cover every finish order
of three parallel calls.

### M3 — Jobs, inbox and preemption

- All three ways a call becomes a job, `tool_pending` and its output files,
  `job_result` delivery through `tool_result` → inbox → `input`, coalescing.
- The wake rule for a turn waiting on calls; `when: now` interrupting a stream;
  `deferred`; `kill`; `run_end.returned`.
- Quiescent shutdown and the final message that closes every
  pending call.
- Plain lines on stdin delivered `turn`; the pid in the `tool_pending`, so jobs
  are stopped with `kill(1)`; `exec`'s `stdin_open` and `write` to a job;
  `AKERN_ASK`.

**Exit:** every timeline in [concurrency.md](concurrency.md#timelines)
is a passing scenario test, and the random stress target runs 10 minutes under
ASan with the invariant checker (`tests/invariants.py`) clean.

### M4 — Sub-agents, session userland, daily driver

- `akern-loop -p`, the positional prompt, child sessions named by call and the
  recursion limit in the kernel; `-p` exiting when done, so a sub-agent with an
  open stdin pipe finishes; cascade shutdown verified across three levels.
- `akern-sessions`, `akern-fork`, `akern-tree`, `akern-compact`.
- Front-end features: context files, prompt templates, auto-compaction.
- `akern` grows from one prompt per line into a usable line mode: shows
  parallel calls and jobs, a key for `when: now`, `/foo` runs `akern-foo`; session
  paths, `--continue` and settings.

**Exit:** a day of real work in `akern` without reaching for another
harness, including a background test run, a mid-turn steer, one sub-agent, one
compaction and one branch.

### M5 — Parity packs

Independent of each other; any order, any author.

- `providers/openai` (OpenAI-compatible endpoints).
- `akern-pkg`.
- MCP bridge tool.
- HTML export.
- A full-screen TUI frontend with a job and sub-agent tree.

**Exit:** every row in [pi-parity.md](pi-parity.md) marked **R** exists;
at least three rows marked **U** have a published example.

## Settled since the first draft

- **Parallel tool execution.** Settled: in the kernel, on by default, capped
  by `--max-processes`.
- **Control channel for third parties.** Settled: removed. The draft's Unix
  socket is gone; whoever starts a kernel drives it over stdin and stdout, a
  parent steers a sub-agent with `write` to its `stdin_open` job, and a second UI on
  a running agent is tmux's job (or, later, a relay program's). Still open:
  programs other than the frontend appending to an owned session.
- **Hooks.** Settled: removed. The kernel has no interception points; a
  wrapping provider, a shim on `PATH` and the front end cover them
  ([design.md](design.md#no-interception-points)).
- **Model switching.** Settled: a `set` command, not a restart, so background
  jobs survive.
- **Job results.** Settled: every finished job wakes the model; no option,
  global or per call, defers or drops a result. Only `abort` defers results
  already queued, so they do not restart the run it stopped.
- **Command line.** Settled: no `-o key=value`. Kernel limits are long flags
  with their units in the name; provider params are `-P key=value`, JSON when
  the value parses, so a typo is a usage error, not a param.
- **Tools versus commands.** Settled: like an OS kernel's core syscalls, the
  agent kernel gives the model `exec` (argv in, `{stdout, stderr, exit}`
  back), `read` and `write`, over files and jobs, compiled in. There is no tool
  directory. Shells, editing and state are the model's business. Everything
  else is a command plus a `SKILL.md`.

## Open questions

Decisions that should be made before or during the milestone noted. Each has a
current leaning so work is not blocked.

### 1. The name (settled)

`akern`, short for agent kernel: the `akern` front end, `akern-loop`, the
`akern-*` tools, the `AKERN_*` variables and `~/.akern`.

### 2. Canonical message shape (M0)

The draft uses Anthropic-shaped content blocks as the common format.
Alternative: a neutral shape of our own. *Leaning:* keep Anthropic's. It is the
superset, the kernel only depends on `tool_use` / `tool_result`, and an
invented format means every provider translates instead of all but one.

### 3. What a message does to running calls (settled)

Settled: nothing is killed or cancelled. A message that arrives while the turn
waits on calls wakes it, and every unfinished call is answered pending and
keeps going as a job, so no work is lost and the model decides whether to
kill. `now` additionally interrupts a streaming response. The draft's
per-message `"inflight":"cancel"` is gone: a user who means "stop" sends
`abort`, or the model kills the job.

### 4. Who decides not to wait (settled)

Settled: the model, per call. `exec` waits by default, and the model passes
`"wait": false` for a program it will not wait for (a server, a watcher, a
sub-agent it steers). There is no arbitrary default cutoff. A call that hangs
is rescued by any message, which answers it pending. Watch for: models
forgetting `wait: false` on servers, which shows up as a turn that stalls
until someone types.

### 5. Pending and job-result wording (M3)

The text the model sees for a pending call, and when the job finishes, has a
large effect on whether it waits, polls in a loop, or forgets. The kernel
records only facts (`tool_pending`, `job_result`); the provider words them,
and a shim on `PATH` can change a program's output.
*Leaning:* write two or three variants and compare them on a small set of
long-running tasks before freezing the defaults.

### 6. Re-adopting jobs after a kernel restart (after M4)

If a kernel dies, its jobs die with it (or are orphaned on a hard kill). A
normal exit closes every `tool_pending` with a `job_result`; after a hard kill
the log shows a `tool_pending` without one. A restarted kernel could re-adopt
survivors if jobs wrote their output straight to files and the kernel tailed
those files instead of pipes. That costs simplicity everywhere for an uncommon
case. *Leaning:* not in v1. Report unfinished jobs on startup as a `notice`, and
let tools that must survive (servers) daemonize themselves.

### 7. Compaction as a branch (M4)

The draft has no compaction entry: a compaction is a new root plus copies of
the kept messages. It keeps the kernel ignorant, at the cost of duplicated
entries and a rule that copies drop conversation-bound blocks such as thinking.
Alternative: a `compaction` entry (`summary`, `first_kept`) that the chain walk
understands, about 20 more kernel lines and no duplication. *Leaning:* start
with branches; switch if the copies cause real trouble with any provider.

### 8. Images and other non-text results (settled)

Settled: the call names the media type. `exec` takes `stdout_type` and
`read` takes `type`, each a MIME type; the kernel returns `image/*` as an
image block and anything else as a document block, and keeps no list of
formats and sniffs no bytes. The Anthropic provider keeps the list its API
accepts and replaces any other type, and any image over the API's limits
(10 MB, 8000 px, or 3000 px when a request has more than 20 images), with a
note, and drops the oldest media first if the request would get too large. Still open: PDF page limits (600, or 100 for
smaller-context models) are not checked, because that means parsing PDFs.

### 9. Hook latency (settled)

Settled: there are no hooks. A shim on `PATH` costs one process for each call
that runs it, and nothing for any other.

### 10. A hook point for responses (settled)

Settled: none. A response is changed by wrapping the provider.

### 11. Where settings live (M4)

The kernel reads flags and environment only. Something has to turn
`~/.akern/config` into those. *Leaning:* the `akern` front end, with a format a shell
can read without a parser (`key=value` lines), so that frontends in any
language can share it.

Project-local skills and system prompts (`./.akern/`, honored only for
trusted directories) are not supported yet: the kernel reads `AKERN_HOME`
alone. The smallest version is the `akern` front end passing `-S` a merged
system prompt when the project is trusted; decide once a real project needs it.

### 12. Hard output caps (settled)

Settled: `exec` takes `stdout_limit` and `stderr_limit` (default 100000 bytes
each; the model can change either). Each stream past the limit is cut and saved
whole to `<session>.d/calls/<call id>.out` or `.err`, and the result adds
`stdout_total` and `stdout_file`. Nothing is lost, so it is a guard rather
than policy. It must be in the kernel: the model cannot cut output after the
fact, because once it is in the context it is too late, and one guard here
covers every program (`cat`, `grep -r`, `find /`, build logs). `read` has its
own bounds for the same reason: 2000 lines and 100000 bytes per call.

### 13. Orphan prevention on macOS (M4)

There is no `PR_SET_PDEATHSIG`. Options: a tiny supervisor per worker that
watches a pipe from the kernel and kills its group on EOF (one more process per
call), or `kqueue` `EVFILT_PROC` watching the parent from a helper. *Leaning:*
accept the gap for v1 and document it; revisit if sub-agent trees make it bite.
