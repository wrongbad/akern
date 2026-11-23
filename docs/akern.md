# akern

The kernel as a command: what it reads, what it writes, and the files it
keeps. How it is built is in the comment atop each file in
[`akern/`](../akern); what it says to a provider is
[providers.md](providers.md); the model's tools are [tools.md](tools.md).

## Conventions

These hold for every stream: stdin, stdout, the provider's pipes and the
session file.

- JSON lines: one complete JSON object per line, UTF-8, ending in `\n`.
- Every object has a string `type`.
- Unknown types and unknown fields are ignored, by everyone. Extension types
  and fields are prefixed `x-`, so they never collide with core names.
- Entry ids are 8 hex characters; `run` and `turn` count from 1; `call` is the
  provider's `tool_use` id, and a job is named by its call id.
- Times are Unix seconds. Paths are as given: a relative `-s` gives relative
  paths in events and entries.

## 1. Environment variables

### Read

| Variable | Default | Meaning |
|---|---|---|
| `AKERN_HOME` | `$HOME/.akern` (`./.akern` without `HOME`) | user layer: `providers/<name>` and `system-prompt`, preferred over the same names in `$AKERN_HOME/system/` (the layer `make install` writes) |
| `AKERN_MODEL` | none | `provider/model` when `-m` is absent ([settings](#settings)). Neither given: exit 2. The kernel never reads `$AKERN_HOME/model`; the `akern-cli` front end turns it into this variable ([tools.md §4](tools.md#4-the-akern-cli-front-end)) |
| `AKERN_RE_LIMIT` | `4` | kernels that may still nest, this one included. `0`, negative or non-numeric: exit 2 |
| `TMPDIR` | `/tmp` | directory of the anonymous session |

### Set for children

Children are the system-prompt program, the provider and the programs tool calls run. These
variables exist so that a child finds the home in use and a nested kernel
picks up where its parent is; everything else passes through unchanged.

| Variable | Value |
|---|---|
| `AKERN_HOME` | the home in use, as resolved above |
| `AKERN_MODEL` | the model in effect |
| `AKERN_RE_LIMIT` | the value read, minus 1 |
| `AKERN_SESSION` | the session path, as given: a sub-agent's session goes in `$AKERN_SESSION.d/agents/` ([§5](#sub-agents)) |

## 2. Arguments

```
akern [-p] [-s session.jsonl] [-m provider/model] [-S system.md] [-P key=value]...
      [--max-processes N] [--kill-grace SECONDS] [prompt...]
```

Flags are parsed with `getopt_long`: `--flag value` or `--flag=value`, and
parsing stops at the first word that is not a flag. An unknown flag, a missing
or bad value, or a `-P` without `key=`: usage on stderr, exit 2.

| Argument | Default | Meaning |
|---|---|---|
| `-p`, `--print` | off | print mode: stdout per [§4](#print-mode); exits after it has run once and is quiescent, without waiting for EOF |
| `-s`, `--session path` | `$TMPDIR/akern-session-XXXXXX` | session file. Created if missing, with its directory; locked through `path.owner`. Owned by another kernel: exit 2 |
| `-m`, `--model provider/model` | `$AKERN_MODEL` | the model ([settings](#settings)). The kernel runs `$AKERN_HOME/providers/<provider>`, else `$AKERN_HOME/system/providers/<provider>`; the request's `model` is the part after `/` |
| `-S`, `--system path` | the system-prompt program's output ([below](#system-prompt)) | system prompt file, read verbatim once at startup; the system-prompt program is not run. Unreadable: exit 2 |
| `-P`, `--param key=value` | | repeatable. Provider params ([settings](#settings)). `value` is sent as JSON when it is a JSON value (`max_tokens=64` is the number 64, `thinking={"type":"adaptive"}` an object), else as a string (`effort=high`) |
| `prompt...` | | words joined by spaces, queued as `source: prompt`, `when: idle`, before stdin is read |

### System prompt

The startup system text is read before the session is opened and before any
signal handler is installed: the `-S` file's bytes, else the output of the
**system-prompt program**, `$AKERN_HOME/system-prompt` if it is executable, else
`$AKERN_HOME/system/system-prompt`. Empty, or no program in either place: there is
none. With no system text, the request's `system` is `null`.

| Rule | |
|---|---|
| run | once, to completion, with no arguments, in the kernel's working directory and environment ([§1](#set-for-children)) |
| stdin | empty |
| stdout | the system text, as opaque bytes |
| stderr | the kernel's stderr |
| failure | a failed `exec`, an exit other than 0, or a signal: `akern: PATH: exit N` (or `signal N`) on stderr, exit 2 |

What the shipped program writes: [tools.md §3](tools.md#the-system-prompt-program).

### Tools

Before the system text, the kernel reads the tool definitions: `$AKERN_HOME/tools.json`,
else `$AKERN_HOME/system/tools.json` ([tools.md §1](tools.md#1-built-in-tools)).
Each failure is a line on stderr and exit 2:

| stderr | When |
|---|---|
| `akern: no tools.json in HOME or HOME/system` | neither file |
| `akern: PATH: <reason>` | unreadable |
| `akern: PATH: not a JSON array` | |
| `akern: PATH: tool N <problem>` | entry N (from 1) is not an object, has no `name` or `program`, is not `shell`, `job` or `edit`, has no `program` (except `job`), or repeats a name |

### Settings

The model (`-m`, else `$AKERN_MODEL`), the system text (`-S`, else the
system-prompt program) and the params (`-P`) are fixed for the kernel's life,
and `hello` reports them. Nothing in the session sets them: `akern -s S` uses
what this start was given, so another model is another start, `akern -s S -m
other/model`, on the same file. A front end that switches models mid-session
lets its kernel settle, closes its stdin, and starts it again
([tools.md](tools.md)); jobs that must survive a switch are not supported.

### Limit flags

| Flag | Default | Value |
|---|---|---|
| `--max-processes N` | `32` | tool calls' programs running at once; one more fails at once with `Not run: already at the --max-processes limit (N).` A whole number, at least 1 |
| `--kill-grace SECONDS` | `2` | from `SIGTERM` to `SIGKILL`; a number, at least 0 |

A stdin line ([§3](#3-stdin)) or provider line longer than 64 MiB is dropped
whole, and the provider killed; a tool call's program whose kept output would
exceed 64 MiB is killed. Unwritten stdout is capped at 8 MiB: past that, provider and tool stdout stop
being read until it drains. If stdout closes, lines still go to the session file and the
kernel carries on.

### Exit

The kernel exits when it is quiescent and either stdin is at EOF or `-p` is
set and a run has started. Quiescent means: no run, no pending abort, no
`now`, `turn` or `idle` item queued, and no running job. A finished job's
result is a `turn` item, so it keeps the kernel alive until it is delivered;
only results deferred by `abort` wait for the next delivery or exit. So
`echo "start the server and run the e2e tests" | akern` waits for the
tests, and a running job (a dev server included) keeps it alive: a front end
that wants jobs killed at EOF sends `{"type":"abort","jobs":true}` first. `-p`
does not wait for EOF, which lets a sub-agent keep an open stdin pipe and still
finish.

Shutdown, whatever its cause: stdin is no longer read; an open run ends
`aborted` as by `abort`, a reply being streamed kept as far as it got;
every child's process group gets `SIGTERM`, then `SIGKILL` after
`--kill-grace` ([process.c](../akern/process.c)). Once all have exited, one
last user message carries the result of every job still open, stdout is
flushed and `S.owner` emptied. A sub-agent is a kernel running as a job, so it
shuts down its own tree when it gets `SIGTERM`; raise `--kill-grace` when
sub-agents are in use to give it time.

| Signal | Effect |
|---|---|
| `SIGINT` | `abort` (jobs kept), [interrupt](#interrupt) |
| second `SIGINT` within 3 s, `SIGTERM`, `SIGHUP` | shutdown; exit 130 |

| Status | When |
|---|---|
| `0` | the last run did not end in `error`, or there was no run |
| `1` | the last run ended in `error`; or a session append failed |
| `2` | bad usage, no model, recursion limit, unreadable `-S` file, failed system-prompt program, session cannot be opened or is owned, fatal system error. `akern: <reason>` on stderr |
| `130` | shutdown by signal |

## 3. Stdin

Lines end with `\n`; a final unterminated line is read at EOF. Each line is
trimmed and empty lines are ignored. A line starting with `{` is a command,
anything else is plain text. Commands act as soon as they are read, whatever
the run is doing. A line longer than 64 MiB is dropped whole, through its
newline, with one command error.

### Plain text

Same as `{"type":"send","content":"<line>"}`: `source: prompt`, `when: turn`.

### Common fields

| Field | Type | Required | Meaning |
|---|---|---|---|
| `type` | string | yes | command name |
| `ref` | any JSON | no | echoed verbatim on the reply |

`content` (below) is a string or an array of content blocks
([providers.md §5](providers.md#5-messages)); a string becomes one text block.

### send

Queue a user item in the inbox.

```json
{"type":"send","content":"use the staging config","when":"now","ref":1}
```

| Field | Type | Required | Default | Meaning |
|---|---|---|---|---|
| `content` | string \| block array | yes | | anything else: error |
| `when` | `now` \| `turn` \| `idle` \| `deferred` | no | `turn` | [delivery mode](#delivery-modes); anything else: error |
| `source` | `prompt` \| `inject` | no | `prompt` | reported in `queued`; anything else: error |

Reply `ok` with `item`, the id the item is queued under, then a `queued` event.

### Delivery modes

| `when` | Delivered | If no run is open |
|---|---|---|
| `now` | as `turn`; also preempts a streaming response | starts a run |
| `turn` | at the next delivery: after a streaming turn ends (with its call answers), or at once when the turn waits on calls, which are then answered pending | starts a run |
| `idle` | when a turn ends without tool calls, before the run ends | starts a run |
| `deferred` | with the next delivery of anything else | stays queued |

| `source` | From | Mode |
|---|---|---|
| `prompt` | a plain line (`turn`), the positional prompt (`idle`), `send` | as sent |
| `inject` | `send` with `source: inject` | as sent |
| `job` | a finished job | always `turn`; `abort` turns those already queued `deferred` |

Every finished job wakes the agent: no option, global or per call, defers or
drops a result. Everything due at one moment goes into one user message in
arrival order, so ten jobs finishing together wake the model once.

### abort

End the current run. The provider gets `SIGTERM`. The turn's unfinished calls
are killed (`Killed: the run was aborted.`); its `wait: 0` calls are answered
pending. Jobs keep running. Whatever is queued when the abort is read is
settled at once, run or not: queued `turn` and `idle` user items (`prompt` and
`inject`) are handed back, in `run_end` when a run is open, else in a
[returned](#returned) event before the `ok`; queued `now` items stay and start
the next run; queued job results stay and become `deferred`, so they start no
run but go with the next delivery, or close their calls at exit. An item sent
after the abort stays queued. So `send` then `abort`, read together, start no
run and hand the prompt back.

```json
{"type":"abort","jobs":true}
```

| Field | Type | Required | Default | Meaning |
|---|---|---|---|---|
| `jobs` | bool | no | `false` | also kill every running job, `wait: 0` calls of the turn included |

Reply `ok`.

### append

Append a session entry for someone else.

```json
{"type":"append","entry":{"type":"x-note","text":"checkpoint"},"ref":"a"}
```

| Field | Type | Required | Default | Meaning |
|---|---|---|---|---|
| `entry` | object | yes | | the entry. `id` and `ts` are assigned by the kernel |
| `entry.type` | string | yes | | entry type |

The entry is appended at once, except a `reset` ([§5](#session-file)): that one
is held until the kernel is idle or a turn ends without tool calls, so a
`tool_use` and its `tool_result` never end up on opposite sides of it. Every
`append` after a held one is held behind it, so entries keep their order.

Reply `ok` with `id`, only when `ref` is given, once the entry is appended.
Error, at once: `entry needs a type`.

### status

Ask for the turn state. It has no fields. Reply: a `status` event, sent even without `ref`.

### Replies

The `ref` is added as the last field. With `-p`, no reply is written.

| Reply | Shape | When |
|---|---|---|
| `ok` | `{"type":"ok","ref":R}`; `send` adds `"item":"8 hex"`, `append` adds `"id":"8 hex"` | success, **only when the command has a `ref`** |
| `status` | [status](#status-1) plus `ref` | `status` |
| `error` | `{"type":"error","where":"command","message":M,"ref":R}`, without `ref` when none | failure; nothing else changes |

| `message` | Cause |
|---|---|
| `invalid JSON` | a line starting with `{` that does not parse (never carries `ref`) |
| `line longer than 64 MiB` | a stdin line over 64 MiB (never carries `ref`) |
| `unknown command` | `type` missing or unrecognized |
| `content must be a string or an array of content blocks` | `send` |
| `when must be now, turn, idle or deferred` | `send` |
| `source must be prompt or inject` | `send` |
| `entry needs a type` | `append` |

## 4. Stdout

Stdout is the kernel's one stream of lines: everything it says goes out here,
in the order it happens, and the same lines go to the session file
([§5](#session-file)). One JSON object per line, each with a `type`. Lines carry `run`, `turn` or `call` where
they apply. A typical run in a new session:

```json
{"type":"session","v":1,"id":"5f2c9a1e","cwd":"/home/k/proj","created":1763470000}
{"type":"hello","protocol":3,"session":"s.jsonl","model":"mock/echo"}
{"type":"queued","item":"5c1e09a4","source":"prompt","when":"turn","n":1}
{"type":"run_start","run":1}
{"type":"message","id":"b03a71d1","ts":1763470001,"message":{"role":"user","content":[{"type":"text","text":"hi"}]},"meta":{"run":1,"source":"inbox","items":[{"id":"5c1e09a4","source":"prompt","at":0,"blocks":1}]}}
{"type":"turn_start","run":1,"turn":1,"id":"6690fe78"}
{"type":"text_delta","text":"echo: hi"}
{"type":"message","id":"6690fe78","ts":1763470002,"message":{"role":"assistant","content":[{"type":"text","text":"echo: hi"}]},"meta":{"model":"mock/echo","stop":"end_turn","run":1,"turn":1,"usage":{"input":1,"output":1,"cache_read":0,"cache_write":0},"source":"assistant"}}
{"type":"turn_end","run":1,"turn":1,"reason":"done","usage":{"input":1,"output":1,"cache_read":0,"cache_write":0},"error":null}
{"type":"run_end","run":1,"reason":"done","returned":[],"items":[],"error":null}
{"type":"settled","run":1}
```

A front end that only shows the answer prints `text_delta` text as it arrives
and stops at `run_end`; a script that wants only the final text uses `-p`.

Lines are of three kinds:

| Kind | Lines | In the session file |
|---|---|---|
| entries | `message`, `replace`, `reset` and whatever `append` adds: lines with an `id` and a `ts` ([§5](#session-file)) | always |
| partial lines | `turn_start` and the provider's deltas, until the reply's `message`; `tool_start` and `tool_progress`, until the call's `block`; `block`s, until the user message that carries them | always |
| everything else | `session`, `hello`, `queued`, `run_start`, `run_end`, `turn_end`, `pending`, `abort`, `preempt`, `interrupt`, `returned`, `settled`, replies, `notice`, `error` | always |

A message streams as partial lines, then is sealed by its `message` line,
which is the record. A front end that missed the partial lines, or reads the
session file instead, loses nothing.

A tool call is open from its `tool_start` until a `block` holding its
`tool_result`. That holds whether the call ends within its turn or runs on as a
job and ends runs later; a front end shows one card per call and closes it on
that block.

### session

The header of a new session file ([§5](#session-file)), its first line. Sent
only when the file was empty; a resumed session already has one.

### hello

Sent once at startup, after the session is opened and before the positional
prompt is queued.

| Field | Type | Meaning |
|---|---|---|
| `protocol` | int | `3`. Protocol 2 kept stdout and the session apart: a `message` event had `run`, `source` and `items` beside the message, the file held only entries, and `turn_end` came after the results. Protocol 1 had `tool_end` where 2 has `block`, and `tool_pending` and `job_result` blocks in the session |
| `session` | string | session path, as given |
| `model` | string | `provider/model` |
| `params` | object | the `-P` params |

### queued

Sent when an item enters the inbox, as it arrives; after the `ok` of the command that sent it.

| Field | Type | Meaning |
|---|---|---|
| `item` | string | the item's id, 8 hex; for `send`, the one its `ok` gave. No entry has it |
| `source` | `prompt` \| `inject` \| `job` | origin |
| `when` | `now` \| `turn` \| `idle` \| `deferred` | delivery mode |
| `n` | int | inbox size, this item included |

### run_start

Sent when a run begins, before its first message.

| Field | Type |
|---|---|
| `run` | int |

### message

A `message` entry ([§5](#session-file)): a reply, sealed with the provider's
`result` message byte for byte, or a user message the inbox delivers.

| `meta` field | Meaning |
|---|---|
| `run` | the run |
| `source` | `assistant`, `inbox`, `results` or `results+inbox`: what the message holds |
| `items` | the inbox items it delivers ([§5](#session-file)); absent when none |
| `model`, `stop`, `turn`, `usage` | on a reply |

### replace

A `replace` entry ([§5](#session-file)) the kernel puts for a block the API
rejected, before the request is sent again.

### turn_start

Sent when the provider is started with the request: the first partial line of
the reply.

| Field | Type | Meaning |
|---|---|---|
| `run` | int | |
| `turn` | int | |
| `id` | string | the entry id the turn's reply will get, 8 hex, so its deltas can be named before the entry exists |

The id is used by the reply the turn seals, also when a preempt or an abort
keeps a partial reply (`meta.stop: aborted`), and kept when the request is
retried on a fresh provider. When this line is in the file, the reply sits in
the context here, where it began ([§5](#session-file)). A turn that seals no
reply (aborted or preempted before any text, provider failed) never uses the
id for an entry. A request retried after a `replace` is a new turn, with a
new `turn_start` and no `turn_end` between.

### Provider events

The provider's deltas and extensions, forwarded verbatim
([providers.md §3](providers.md#3-output)): `text_delta`, `thinking_delta`,
`tool_use_start`, `tool_use_delta`, `x-*`. They have no `run` or `turn` and
are partial lines of the open reply. A provider `error` line is forwarded too
([error](#error)). The `result` line is not forwarded, and lines of any other
type are dropped: they would be lines of the session, which only the kernel
writes.

```json
{"type":"text_delta","text":"I'll "}
```

### tool_start

Sent once per `tool_use` block, when the call is under way: once its program
has started, or, for a call that fails at once, just before its `block`.

| Field | Type | Meaning |
|---|---|---|
| `run`, `turn` | int | |
| `call` | string | `tool_use` id |
| `name` | string | tool name |
| `input` | object or string | `{}` when missing or empty; a string when the model's input was not a JSON object (the call then fails) |
| `pid` | int | its process; absent when none runs (a `job` call, or one that failed before starting) |
| `out` | string | with `pid`: the call's output file, its program's `AKERN_OUT` ([§5](#layout)); akern-shell writes all of the output there |
| `stdin_open` | bool | with `pid`: its stdin stays open for `job` input (`eof: false`) |

### tool_progress

Sent for each chunk of a tool call's output (stdout and stderr share a pipe), including after its call is answered pending; not for an image or PDF. Chunks are not split on line boundaries.

| Field | Type | Meaning |
|---|---|---|
| `run` | int | the call's run |
| `call` | string | |
| `data` | string | the chunk |

### block

Sent when a block of a coming user message is final: a call's `tool_result`,
as the call ends. A call that ends within its turn goes in that turn's results
message; a job's result is queued and goes in the next delivery
([delivery modes](#delivery-modes)), or at exit. Either way the `message` that
carries it comes later, with the block exactly as announced.

```json
{"type":"block","run":1,"block":{"type":"tool_result","tool_use_id":"tu_1","is_error":false,"content":[{"type":"text","text":"[exit 0]\nok\n"}]}}
```

| Field | Type | Meaning |
|---|---|---|
| `run` | int | the call's run |
| `block` | object | the block as it will be stored: for a job, with `name`, `status` and `ran` ([§5](#session-file)) |

### turn_end

Closes every turn: after its reply, or at once when the provider failed or
could not start. The calls the reply asks for start after it.

| Field | Type | Meaning |
|---|---|---|
| `run`, `turn` | int | |
| `reason` | `done` \| `aborted` \| `error` | `aborted`: stopped by a preempt or an abort |
| `usage` | object \| null | the provider's `usage` |
| `error` | object \| null | with `reason: error`, as `run_end.error` |

### pending

Sent when a turn's results go out while some of its calls still run: those
calls are jobs from now on, answered when they end. When none of the calls had
ended, it is the only line that answers the turn.

| Field | Type | Meaning |
|---|---|---|
| `run` | int | |
| `calls` | array | their call ids |

### abort

Sent for each `abort` command, `SIGINT` and shutdown that finds work in
flight: the reply stops, the turn's waited calls are killed, and, with `jobs`,
every job ([§3](#abort)).

| Field | Type | Meaning |
|---|---|---|
| `jobs` | bool | jobs are killed too |

### preempt

Sent when a message delivered while a reply streams (a `now` item) cuts the
reply short ([delivery modes](#delivery-modes)), after that message.

| Field | Type | Meaning |
|---|---|---|
| `run`, `turn` | int | the interrupted turn |

### interrupt

Sent for each `SIGINT` that does not shut down, idle or not, before the events of the abort it starts ([§2](#exit)).

| Field | Type | Meaning |
|---|---|---|
| `quit_within` | int | seconds in which another `SIGINT` shuts down |

### run_end

Sent when a run ends.

| Field | Type | Meaning |
|---|---|---|
| `run` | int | |
| `reason` | `done` \| `aborted` \| `error` | |
| `returned` | array | contents (string or block array) of the queued user items (`prompt`, `inject`) handed back by `abort` or shutdown; job results are never handed back |
| `items` | array | the ids of those items, in the same order |
| `error` | object \| null | with `reason: error`: kernel error `{"where","message"}`, or the provider's `error` line as sent; otherwise `null`. A provider `result` after an `error` line wins |

### returned

Sent when an `abort` read while no run is open hands queued user items back,
after its `abort` line and before its `ok`. Not sent when it hands nothing back.

| Field | Type | Meaning |
|---|---|---|
| `returned` | array | contents of the items handed back, as in `run_end` |
| `items` | array | their ids, in the same order |

### settled

Sent each time the kernel becomes quiescent after working ([§2](#exit)): no
run, nothing due in the inbox, no job running. A front end that waits for all
of the work a prompt started, a job's result included, waits for this.

| Field | Type | Meaning |
|---|---|---|
| `run` | int | the latest run |

### status

The reply to `status`.

```json
{"type":"status","state":"waiting","run":1,"turn":2,"model":"mock/echo","inbox":0,
 "calls":[{"call":"tu_2","name":"shell","state":"running"}],
 "jobs":[{"call":"tu_1","name":"shell","input":{"command":"make","wait":0},"pid":27798,"state":"running"}]}
```

| Field | Type | Meaning |
|---|---|---|
| `state` | `idle` \| `stream` \| `aborting` \| `waiting` | turn state; `waiting`: on this turn's calls |
| `run`, `turn` | int | latest; `0` before the first |
| `model` | string | |
| `inbox` | int | queued items |
| `calls` | array | this turn's calls, until they are answered: `{"call","name","state"}`, `state` one of `running`, `done`, `cancelled` |
| `jobs` | array | live jobs (pending and `wait: 0` calls): `{"call","name","input","pid","state"}`; `pid` is `0` when no process runs |
| `ref` | any | when given |

### ok

The reply to a successful command that carries a `ref` ([§3](#replies)).

| Field | Type | Meaning |
|---|---|---|
| `item` | string | only for `send`: the id of the item it queued |
| `id` | string | only for `append` |
| `ref` | any | |

### notice

Sent for each line the provider writes to stderr.

| Field | Type | Meaning |
|---|---|---|
| `source` | string | `provider` |
| `message` | string | the line |

### error

Kernel errors:

| Field | Type | Meaning |
|---|---|---|
| `where` | `provider` \| `kernel` \| `command` | |
| `message` | string | |
| `ref` | any | `command` errors only, when given |

| `where` | `message` | Then |
|---|---|---|
| `provider` | `no provider for <model> in <home>/providers or <home>/system/providers` | run ends `error` |
| `provider` | `invalid line from provider` | line dropped |
| `provider` | `provider exited without a result` | run ends `error`, or continues if inbox items are due; not sent if the provider already wrote an `error` line |
| `kernel` | `session append failed; stopping` | shutdown, exit 1 |
| `command` | [§3](#replies) | none |

A provider failure is forwarded as the provider wrote it, with no `where`:

```json
{"type":"error","kind":"rate_limit","message":"slow down","status":429}
```

One that names a rejected image or document (`kind: invalid_block`,
[providers.md §4](providers.md#4-errors)) does not end the run: a `replace`
event follows and the request is sent again, as a new turn, up to 8 times per
request. A block that is no longer an image or document, or a ninth one, ends
the run `error` like any other failure.

### Print mode

With `-p` none of the lines above go to stdout (the session file still gets
them all). Instead:

| Stream | Content |
|---|---|
| stdout | at each run end, the text of the run's last assistant message (text blocks joined) and `\n`; nothing when empty |
| stderr | at a run end with `reason: error`: `akern: <error.message>`; startup failures as in [§2](#exit) |

## 5. Files

### Home

`$AKERN_HOME` is read, never written. `build/` has the same layout
(tools.md §3):

```
providers/<name>                   yours: the program -m <name>/… starts (providers.md)
system-prompt                      yours: the program whose output is the system prompt (§2)
system.md, skills/<name>/          yours: read by the shipped system-prompt, not the kernel (tools.md §3)
system/                            what `make` builds and `make install` copies:
  providers/<name>                 used where providers/<name> above is missing
  system-prompt                    used where system-prompt above is missing (tools.md §3)
  skills/<name>/SKILL.md           listed by the shipped system-prompt
```

### Layout

Everything a session produces sits next to its file, named after it:

```
S                                  the log: stdout, as said; append-only
S.owner                            lock; {"pid":N} while a kernel holds S, empty after
S.d/calls/<call>.out               a call's whole output (every shell call's)
```

| Path | Created | Removed |
|---|---|---|
| `S` | at open, if missing (mode 0644; the anonymous session is created by `mkstemp`, mode 0600), with a `session` header line | never |
| `S.owner` | at open | never; emptied at exit |
| `S.d/` and its subdirectories | on first use | never |
| `calls/` files | by the call's program (akern-shell: for every shell call) | never |

A `calls/` file that cannot be created or written is not written further; the
call's result says so with the system's reason
([tools.md](tools.md#shell)), and the file keeps only what was written before.

`<call>` is the `tool_use` id with every character but letters, digits and `-`
replaced by `_`. The anonymous session (no `-s`) is
`$TMPDIR/akern-session-XXXXXX`, and its `.owner` and `.d` sit beside it in
`$TMPDIR`.

### Session file

The session file is the kernel's stdout ([§4](#4-stdout)): every line the
kernel writes goes to it, as written, in the same order. Line 1 is the header;
the lines with an `id` and a `ts` are its **entries**, and the rest (`hello`,
`queued`, `run_start`, deltas, `turn_end`, replies, …) record what happened
around them. A reader that wants only the record skips the partial lines
([§4](#4-stdout)).

```json
{"type":"session","v":1,"id":"5f2c9a1e","cwd":"/home/k/proj","created":1763470000}
{"type":"hello","protocol":3,"session":"s.jsonl","model":"anthropic/claude-opus-5-5","params":{}}
{"type":"queued","item":"9d0e1f2a","source":"prompt","when":"turn","n":1}
{"type":"run_start","run":1}
{"type":"message","id":"a1b2c3d4","ts":1763470001,"message":{"role":"user","content":[…]},"meta":{…}}
{"type":"message","id":"e5f6a7b8","ts":1763470009,"message":{"role":"assistant","content":[…]},
 "meta":{"model":"anthropic/claude-opus-5-5","stop":"tool_use","run":1,"turn":1,
         "usage":{"input":812,"output":64,"cache_read":0,"cache_write":812},"source":"assistant"}}
{"type":"turn_end","run":1,"turn":1,"reason":"done","usage":{…},"error":null}
```

| Entry field | Required | Meaning |
|---|---|---|
| `type` | yes | `message`, `replace`, `reset`, or anything else |
| `id` | yes | unique within the file |
| `ts` | yes | when it was appended |
| `message` | for `message` | a message ([providers.md §5](providers.md#5-messages)), stored verbatim |
| `meta` | no | facts not sent to the model; the kernel writes `{run, source}` on every message, `{model, stop, turn, usage}` on a reply, and `items` on a user message that delivers inbox items ([§4](#message)) |

| Rule | |
|---|---|
| context | the `message` entries after the last `reset` entry (all of them when there is none), each sending its `message`, or that of the last `replace` entry in the file that targets it |
| order | each message sits at the first line that carries its `id`: a reply whose `turn_start` is in the file sits there, where it began, even when a message delivered while it streamed was sealed first; every other message sits at its own line |
| other types | add nothing to the context; that is how extensions keep state beside the conversation |

`meta.items` lists the inbox items a user message delivers, in the order their
blocks appear, so a reader can split a message that delivered several items
back into them:

| Field | Meaning |
|---|---|
| `id` | the item's id, as on its `ok` and `queued`; never an entry's id, nor another item's in the file |
| `source` | `prompt`, `inject` or `job` |
| `at` | the index of its first block in `content`; blocks before the first item are the turn's call results |
| `blocks` | how many blocks it is |

Tool calls in messages:

| Rule | |
|---|---|
| answers | each `tool_use` gets exactly one `tool_result`, in a later user message |
| in its turn | a call that ended by the time its turn was answered has its `tool_result` in the user message right after its assistant message |
| a job | a call still running then (answered pending: its `wait` ran out, a message arrived first, or `wait: 0`) has no block there. Its `tool_result` comes in a later user message when it ends, with three more fields: `name`, `status` (`exit N`, `signal N`, `killed`, `not run`) and `ran` (seconds since its program started, rounded; `0` if it never started) |
| placeholders | not in the session: a provider answers each call still running with a placeholder of its own and sends a late `tool_result` as text, or as a native asynchronous result where its API has one ([providers.md §5](providers.md#5-messages)) |
| protocol 1 | sessions written before kept a `tool_pending` block where a job is now unanswered, and its late result as a `job_result` block; providers still read both |

### Replace, reset and other entries

A `replace` entry changes what a message entry sends from then on, without
rewriting the file:

```json
{"type":"replace","id":"c9d0e1f2","ts":1763470012,"target":"a1b2c3d4",
 "message":{"role":"user","content":[{"type":"text","text":"look"},{"type":"text","text":"[image omitted: image exceeds 5 MB maximum]"}]}}
```

| Field | Meaning |
|---|---|
| `target` | the `id` of a `message` entry; anything else, or an id not yet in the file, is ignored |
| `message` | the whole message to send in its place: an object; anything else is ignored |

The last one in the file for a target wins, after a resume and in a fork that
holds it. The kernel appends one when a provider names a block the API rejected
([providers.md §4](providers.md#4-errors)); a front end appends one with
`append`, to redact a secret for example. The original message stays in the
file.

Conventional entry types, written by commands and front ends; the kernel
treats them like any other:

| Type | Meaning |
|---|---|
| `name` | `{"name":"…"}`, the session's display name |
| `label` | `{"target":"<id>","label":"…"}`, a bookmark on an entry |
| `x-*` | extension state |

A `reset` entry needs no other fields; [§3](#append) says when one sent with
`append` lands. Compaction is not an entry type: append a `reset`, then a
`message` entry whose message is the summary, then re-append the messages to
keep (`meta.copy_of` records the original id). Everything before the `reset`
stays in the file. Copies drop blocks that are only valid in the conversation
that produced them (thinking blocks, on providers that bind them); the provider
documents which.

### Writing

| Session | Who appends |
|---|---|
| owned (a kernel holds a write lock on `S.owner`) | only that kernel. Whoever drives it sends `append` on its stdin ([§3](#append)) |
| not owned | anyone: one `write(2)` of one complete line, appended under an exclusive `flock` on `S` |

Never rewrite or truncate: append-only is what makes concurrent readers safe
and prompt caches stable.

### Session lifecycle

| Step | What happens |
|---|---|
| open | create `S` if missing; take the lock on `S.owner` (held by another kernel: exit 2, naming its pid); write `{"pid":N}` into it; read the file once, rebuilding the context; write the header if `S` was empty |
| run | every line the kernel writes to stdout is appended to `S`, as written |
| exit | `S.owner` is emptied; the lock goes with the process |
| crash | the lock goes with the process; `S.owner` keeps a dead pid, which the next open overwrites |
| resume | `akern -s S`: the context is rebuilt from the file, with this start's settings; items it shows `queued` but never delivered are not queued again |

A `tool_use` with no `tool_result` anywhere after it was running when its
kernel was killed with `SIGKILL`; any other exit answers it. Its files keep
whatever it wrote.

### Forks

A session cannot branch in place. A fork is a new file holding the first lines
of one: `head -n N S > F && akern -s F` continues after line `N` (the header and
the first `N - 1` entries); `cp S F` forks at the end. The fork shares that
history and then diverges. Paths in its messages (output files) still point at
`S.d/`. A fork that ends between a `tool_use` and its `tool_result` leaves the
call unanswered, as after `SIGKILL`.

### Sub-agents

A sub-agent is `akern -p` on a child session, run by a `shell` call
exactly as a person would run it; there is no wrapper command. The parent
names its session with `-s`; without one, it is anonymous. Its files follow the
same layout, next to that path:

```
s.jsonl                            parent; the call's tool_use holds the child's command
s.jsonl.d/calls/tu_1.out           the sub-agent's -p answer, as the job's stdout
s.jsonl.d/agents/rename.jsonl      the sub-agent's whole conversation (-s "$AKERN_SESSION.d/agents/rename.jsonl")
s.jsonl.d/agents/rename.jsonl.d/…  its own calls' output files
```

| Step | What happens |
|---|---|
| start | `shell {"command":"akern -p -s \"$AKERN_SESSION.d/agents/rename.jsonl\" '…'","wait":0,"eof":false}`; the directory is created. Beside the parent's session, nothing lands in the project. `wait: 0` lets the parent keep working; without it, the parent waits for the answer until a message arrives |
| model | the parent's: `AKERN_MODEL` is inherited, unless the command passes `-m` |
| run | the child appends to its own file only; the parent sees it as a job (`tail` the child's file to watch) |
| steer | `job {"id":"<call id>","stdin":"…\n"}` to the child's stdin: a plain line is a `turn` item for it ([delivery modes](#delivery-modes)), `{"type":"send","when":"now","content":"…"}` interrupts it. Each level steers the one below the same way |
| end | the child exits once quiescent (`-p` does not wait for EOF); its final answer is the job's stdout, in the job's late `tool_result`. Messages up from a child mid-run are not supported: it reports once, at the end |
| resume | the same `-s` again continues the child's conversation |

Recursion is bounded by `AKERN_RE_LIMIT` ([§1](#1-environment-variables)),
which every nested kernel inherits however it is reached (directly, through
a script, a shim); nobody inspects `shell` calls. With the default 4, a
top-level kernel can have three levels of sub-agents below it. Each
sub-agent's `tool_use` holds its command, `-s` included, so a front end can walk
the tree from session to session. Two kernels given the same `-s` at once: the
second exits 2.

An agent that is not akern but speaks ACP protocol 2 is a sub-agent the same
way, through `akern-agent` ([tools.md §2](tools.md#akern-agent)): the same
`shell` call shape, the same steering lines and the same `-s` to continue it.
Its session file holds the ACP connection rather than akern entries, and the
agent keeps its own conversation. `AKERN_RE_LIMIT` still counts down through
any akern below it, as with any other program.
