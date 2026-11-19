# 03 — Concurrency

How one single-threaded C process runs parallel tool calls, background jobs,
sub-agents and steering messages that cut into a turn. This document defines the model;
the wire formats are in [protocols.md](protocols.md) and the
implementation in [kernel.md](kernel.md).

## The one rule

> **The conversation is serial. The work is concurrent.**

A session has at most one model request in flight. A conversation is a linear
chain of messages, and two requests from the same head would produce two
branches. Everything the model *launches* runs concurrently: with the next
request, with each other, and with the user typing.

Parallelism across conversations is not a kernel feature. It comes from running
more kernels: a sub-agent is another `akern-loop` on another session file.

## Moving parts

```
                      ┌─────────────────────── akern-loop ───────────────────────┐
 stdin ──► commands ─►│   inbox ──► DRIVER (one per session) ──► session.jsonl   │──► events ─► stdout
                      │     ▲          │ spawns / cancels                        │
                      │     │          ▼                                         │
                      │     │       WORKERS: provider · calls · jobs             │
                      │     └──────────── job results, injected messages ────────┘
                      └──────────────────────────────────────────────────────────┘
```

| Part | What it is | How many |
|---|---|---|
| **Driver** | The state machine that owns the conversation: builds requests, appends messages, decides when a turn ends | one |
| **Worker** | A child process and the kernel's bookkeeping for it | one per provider and running tool; tools capped by `--max-processes` (default 32) |
| **Call** | The life of one `tool_use` block: waiting for a slot, the tool, its result | one per block, all running in parallel |
| **Job** | A call the model is not waiting for: one answered pending, or one started with `"wait": false`. Same process, same call; named by its call id | any number |
| **Inbox** | Everything waiting to enter the conversation: user prompts, steering, job results, injected messages | one queue |

The kernel's single thread does three things: read lines from stdin and its
children, advance the driver, and write lines to children and the log. Nothing it
does waits for anything else. A permission shim waiting on a human stalls that
one call, and everything else keeps moving.

## Invariants

These hold at every instant, under every interleaving. Tests are written
against them.

1. **One request in flight.** At most one provider process per session.
2. **Every call is answered before the next request.** Each `tool_use` in an
   assistant message is answered in the following user message, in call
   order: by a `tool_result` (its result, or an error saying it was cancelled)
   or by a `tool_pending` (it is still in progress).
3. **Every pending call is closed once.** Each `tool_pending` is followed by
   exactly one `job_result` in a later user message, also across shutdown.
4. **One writer.** While a kernel runs, only it appends to the session. Its
   frontend sends `append` on stdin ([protocols.md §2](protocols.md#writing)).
5. **Append-only.** Preemption, cancellation and branching never rewrite
   history. They only add entries.
6. **Every child dies.** Every process the kernel starts is either reaped or
   killed before the kernel exits, including the whole process tree of a
   sub-agent.
7. **Nothing reaches the model unannounced.** Every message appended to the
   log is also emitted as a `message` event, so a frontend can always rebuild
   exactly what the model saw.

## Calls and jobs

All `tool_use` blocks in an assistant message start at once. Each call moves
through these states:

```
 waiting ──► running ──► done
 (for a      (tool
  slot)      process)
    └───────────┴──► cancelled (abort)
```

`read` and `write` run inside the kernel: they skip `waiting` and `running`
and are `done` as soon as they start, as is a call to an unknown tool.

While its calls run, the driver is in `waiting`. It wakes when either:

- every call it waits for is `done` or `cancelled`, and every call with
  `"wait": false` has at least started (`running` or later), so its answer can
  name a pid and files; or
- a `now` or `turn` inbox item queued since it began waiting is due: a typed
  line, a `send`, or an older job's result (always `turn`).
  `idle` and `deferred` items do not wake it, and neither do items queued
  earlier, such as a line typed while the model streamed: those wait for the
  calls and go with their results.

It then appends one user message: for each call in call order, a
`tool_result` if the call is settled, otherwise a `tool_pending` with its
state ([protocols.md §1](protocols.md#1-messages)); then the inbox items due.
So `exec` never holds a turn hostage: a call that hangs is answered pending as
soon as anything else needs the model.

### Two ways a call becomes a job

Both lead to the same mechanism, so the model and the user each have a lever:

| Trigger | Who decides | Example |
|---|---|---|
| `"wait": false` on the call | the model, per call | `exec {"argv":["npm","run","dev"],"wait":false}` starts a dev server; the turn goes on once it runs |
| A message arrives while the call is unfinished | the user, or another job | a line typed during a long test run; the turn wakes and the run is answered pending |

A `wait: false` call is a job from the start, and so is any call answered
with a `tool_pending`. A `wait: false` call that finished before the request
was built gets an ordinary `tool_result`. The
kernel records facts and the provider words them: the `tool_pending` carries
the pid, the output files and whether stdin is open, and the provider renders
that into something its API accepts
([protocols.md §4](protocols.md#4-provider)).

The process keeps running. Its stdout and stderr are still collected, and also
written to `<session>.d/calls/<call id>.out` and `.err`, so the model can
`tail` them at any time. To wait for it, the model ends its turn (the result
starts a new one) or runs `while kill -0 PID; do sleep 1; done`. When it
exits, its result goes into the
**inbox** with `source: "job"`, the same as anything else that wants to enter
the conversation: one `job_result` block naming the call, its status and how
long it ran. The kernel writes no prose for it.

A job's lifetime is not tied to a turn or a run. It ends when it exits, when it
is killed (`kill -TERM -- -PID` from the model or a person, or the `kill`
command on stdin, `{"type":"kill","job":"tu_07"}`), or when the kernel shuts
down.

### Talking to a running program

With `stdin_open: true`, the kernel does not close the program's stdin pipe
after writing the call's `stdin` text (if any). The `tool_pending` says
`"stdin_open": true`, and the model sends more with
`write {"job":"<call id>","content":"text\n"}`, reading replies from the
job's `.out` file; `close: true` ends input. The pipe is anonymous, so there
is nothing to clean up. This is how the model steers a sub-agent (below) and
drives any interactive program: `python3 -i`, `psql`, `gdb`.

### Limited calls

`--max-processes` caps running tools. Calls beyond the cap stay in `waiting`. The
provider is always admitted, so the conversation cannot deadlock behind its
own jobs.

## The inbox and delivery modes

Every message that does not come from the model goes through the inbox. Each
item has a `source` (`prompt`, `job`, `inject`) and a `when`:

| `when` | Delivered | If no run is active |
|---|---|---|
| `now` | As `turn`, and it also interrupts a streaming response (see below) | starts a run |
| `turn` | At the next turn boundary, merged after that turn's tool results. A turn waiting on calls wakes for it at once, its unfinished calls answered pending. If the model has just ended its turn, a new turn starts with it | starts a run |
| `idle` | When a turn ends without tool calls, before the run settles | starts a run |
| `deferred` | Held until something else starts or continues a run, then delivered along with it | stays held |

A plain-text line on stdin is `turn`, so typing while the agent works steers
it at the next turn boundary, or at once if it is waiting on tools (pi's
Enter-while-streaming), and typing while it is idle starts a run at once. Defaults per command:
[akern-loop.md §3](akern-loop.md#3-stdin). Each item is queued as it arrives;
a frontend that wants to rewrite a prompt does so before sending it.

**Every finished job wakes the agent.** A job result is always queued `turn`;
no option, global or per call, defers or drops it. The one exception is
`abort`: results already queued become `deferred`, so they do not restart the
run that was just stopped. Jobs that finish after it wake the agent as usual.

**Coalescing.** All items due at the same moment go into one user message, in
arrival order. Ten jobs finishing together wake the model once, not ten times.

## Preemption

`now` interrupts one thing: a streaming response. The goal: the model sees the
new message as soon as possible, and nothing already done is thrown away.

The kernel sends the provider `SIGTERM` and emits `preempt`. The provider ends
with `stop: "aborted"` and only the complete text and thinking blocks it has,
and **no tool calls**. The kernel appends that partial message if it is
non-empty, appends the item (with any other `now` and `turn` items due), and
requests again.

Nothing else is interrupted. A turn waiting on calls wakes for it as for
`turn`.

So **`now` never loses work in progress.** A 10-minute test run that was
underway is answered pending, and a call still waiting for a slot is pending
too and runs when it can. If the user's message
was "stop, that's the wrong directory", the model kills the job itself. If it
was "also check the docs", the job's result arrives later.

`abort` is different. It ends the run. The turn's unfinished calls are killed,
or cancelled if not yet started; `wait: false` calls of the turn are answered
pending. User items with `when` of `turn` or `idle` are returned to the
frontend in the `run_end` event, so it can put them back in the editor. Job
results already queued become `deferred`: they start no run of their own and
go with the next delivery, or close their calls at shutdown. Jobs
keep running unless the command says `"jobs": true`.

## Timelines

Time flows down. `→` is an event the frontend sees.

**Parallel calls, one not waited for.** The model calls
`exec(npm run dev)` with `"wait": false` and `read(src/app.ts)` together.

```
provider result: [tool_use t1 exec, tool_use t2 read]     → message, tool_start t1, tool_start t2
t2 finishes at once, in the kernel                        → tool_end t2
t1 is running: the turn wakes
user [tool_pending t1 running pid out err, tool_result t2] → message, turn_end
request 2 streams …                                       (t1 still running)
t1 exits 0; inbox(job, when=turn)                         → tool_end t1, queued
request 2 ends without tool calls; turn boundary:
  user [job_result t1]                                    → message, turn_start
```

**Steer `now` while the model is streaming.**

```
request 3 streaming: "I'll rename the module and then…"
user: send {when:"now", "use snake_case"}                  → queued
SIGTERM provider                                           → preempt
provider emits result stop=aborted, text only
append assistant [text "I'll rename the module and then…"] → message
append user [text "use snake_case"]                        → message
request 4                                                  → turn_start
```

**A line typed while calls run.** `t5` is running and `t6` is waiting for a
slot (`--max-processes` reached).

```
user types "skip the migration"                            → queued
the turn wakes: t5 running, t6 waiting
append user [tool_pending t5 running pid out err, tool_pending t6 waiting,
             text "skip the migration"]                    → message, turn_end
request: the model sees t5 is still running and may kill its group
a slot frees; t6 runs and exits                            → tool_end t6, queued
… user [job_result t6] at the next turn boundary           → message
```

**A job finishes while the session is idle.**

```
(idle; t3 = a build started with wait:false)
t3 exits; inbox(job, when=turn)                            → tool_end t3, queued
inbox has a turn item, no run active → start run           → run_start
append user [job_result t3]; request                       → message, turn_start
```

## Sub-agents: recursion through the CLI

A sub-agent is `akern-loop -p` on a child session file. The model runs it
through `exec`, exactly as a person would:

```
model:      exec {"argv": ["akern-loop", "-p", "-s", "agents/callers.jsonl",
                           "find every caller of parse_header"],
                  "wait": false, "stdin_open": true}
akern-loop: session agents/callers.jsonl, named by the parent
            runs the prompt; stdin is the call's pipe, kept open
            exits once it has run and is quiescent (-p does not wait for EOF)
            stdout: only its final answer, which becomes the job's result
```

There is no wrapper command. A wrapper exists only because an API is missing,
so the missing pieces (print mode, the recursion limit, a stdin that stays
open) are in `akern-loop` and `exec` instead.

- `wait: false` makes the call a job as soon as it starts, so the parent
  model keeps working and receives the final answer as its `job_result`. The
  child's whole history is in its session file, which the parent can `tail`.
- Steering is a line written to the child's stdin,
  `write {"job":"<call id>","content":"line\n"}`: plain text reaches it at its
  next turn boundary (at once if it is waiting on tools), `{"type":"send","when":"now","content":"focus on the
  parser"}` interrupts it. Each level steers the one below the same way.
  A person steers the top-level agent by typing into its terminal.
- Messages up from a child mid-run are not supported, on purpose: a sub-agent
  reports once, at the end.
- Without `wait: false`, the parent waits for the sub-agent's answer, until a
  message arrives. A person or script gets the same with `akern-loop -p "…"`.
- The child uses the parent's model (`AKERN_MODEL` is inherited) unless the
  call passes `-m`.
- Recursion is bounded by one countdown in the environment,
  `AKERN_RECURSION_LIMIT` ([akern-loop.md §1](akern-loop.md#1-environment-variables)).
  Nobody inspects `exec` calls:
  however a nested kernel is reached (directly, through `sh -c`, a script, a
  shim), it inherits the count. With the default, a top-level kernel can have
  three levels of sub-agents below it.
- The parent names the child's session, so it can read the child's history
  or continue it later with the same `-s`. The tree of kernels is
  discoverable: each sub-agent's `tool_use` holds the child's argv, `-s`
  included, so a frontend can walk from session to session.

## Cancellation is a tree

Each call runs in its own process group, so `kill(-pgid)` reaches a runaway
`make -j` and everything under it. A sub-agent's tools are in *their own*
groups under the child kernel, which the parent's signal does not reach.
Shutdown therefore cascades:

1. A kernel that receives `SIGTERM` sends `SIGTERM` to every worker group it
   owns, waits up to its grace period (default 2 s), sends `SIGKILL` to the
   survivors, and exits.
2. The grace period is `--kill-grace`. Raise it when sub-agents are in
   use, so a child kernel has time to clean up its own tree first.
3. `SIGKILL` of a kernel cannot cascade. On Linux, children are started with
   `PR_SET_PDEATHSIG` as a backstop. On macOS a worker's stdin is a pipe from
   the kernel, so well-behaved tools notice EOF (with `stdin_open` too: the
   kernel holds the only write end). Orphans remain possible there,
   which is a known gap.

## Shutdown and end of input

When stdin closes, the kernel keeps working until it is **quiescent**, then
exits. Quiescence: [akern-loop.md §2](akern-loop.md#exit).

So `echo "start the server and run the e2e tests" | akern-loop` waits for the
tests and lets the model respond to their results. A running job, a dev server
included, keeps the process alive; a frontend that wants it gone at EOF sends
`{"type":"abort","jobs":true}` before closing stdin.

`-p` does not wait for stdin to close: once the kernel has run at least once
and is quiescent by the same rule, it exits. That is what lets a sub-agent keep
an open stdin pipe and still finish.

On the way out, once every worker has exited, the kernel appends one last user
message that closes every open `tool_pending`: each job result still in the
inbox, whatever its `when`, and a `job_result` for each pending call that had
not finished. That one carries the result the call would have had on its own
exit (an image or document with `stdout_type`, spilled output past its limits)
or, if it never started, `Not run: the kernel stopped.` with `ran` 0.

## Stress points, and what answers them

| Problem | Where it would break | Answer |
|---|---|---|
| A tool call without a result when the turn is interrupted | the provider API rejects the next request | invariant 2: a `tool_result` or a `tool_pending`, always, and the provider renders the latter as something its API accepts |
| A call that hangs, or a server started without `wait: false` | a turn that never ends | any `now` or `turn` item wakes the turn; the call is answered pending |
| A partial assistant message with half a tool call | invalid history | provider contract: aborted results carry no `tool_use`. The kernel also strips any it finds |
| Unsigned partial thinking blocks | the provider rejects the replay | the provider drops incomplete thinking from aborted results |
| Someone else appending to the session while the kernel appends | interleaved or broken chain | one writer. Only the kernel's stdin can append to an owned session, and the kernel serializes |
| Compaction or branching while calls are in flight | results land on a chain that no longer holds their `tool_use` | structural appends sent on stdin wait for the next boundary |
| Job results arriving after the conversation branched or compacted | a result refers to a call that is no longer in context | results are self-describing `job_result` blocks (call id, name, status), which the provider renders as its API allows: the Anthropic one as text, never as `tool_result` |
| A command that prints far more than the model needs (`cat` of a log, `grep -r`) | the context floods, and once the output is in it, it is too late | `stdout_limit` and `stderr_limit`: the result keeps the first 100000 bytes of each stream and names a file holding the whole of it |
| Interleaved output from many children | frontend confusion | every event carries `run`, `turn` or `call` ids |
| Ten jobs finishing at once | ten wake-ups, ten requests | coalescing at delivery |
| Two concurrent shims both asking the user | garbled terminal | `AKERN_ASK` helpers serialize prompts; the reference one takes a lock on the tty |
| Shims for parallel calls run concurrently | shims sharing temp files | contract: a shim must be reentrant |
| Runaway fan-out (model launches 200 calls) | fork bomb | `--max-processes`; excess calls stay in `waiting` |
| Model switch would need a restart, which kills jobs | lost background work | a `set` command changes model and params for the next request; no restart |
| Prompt cache churn from injected messages | cost | injections are appends at the tail, so the cached prefix is never disturbed |
| Kernel stopped mid-job | orphans, lost results | shutdown closes every `tool_pending` with a `job_result`; output is in files. After `SIGKILL` a `tool_pending` stays open. Re-adopting running jobs after a restart is an open question ([roadmap.md](roadmap.md)) |
