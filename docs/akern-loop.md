# akern-loop

The kernel as a command: what it reads, what it writes, and the files it
keeps. How it behaves inside (delivery modes, the wake rule, jobs) is
[kernel.md](kernel.md); what it says to a provider is
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
| `AKERN_HOME` | `$HOME/.akern` (`./.akern` without `HOME`) | holds `providers/<name>` and `system.md` |
| `AKERN_MODEL` | none | `provider/model` when `-m` is absent. Neither given: exit 2. The kernel never reads `$AKERN_HOME/model`; the `akern` front end turns it into this variable ([tools.md §4](tools.md#4-the-akern-front-end)) |
| `AKERN_RECURSION_LIMIT` | `4` | kernels that may still nest, this one included. `0`, negative or non-numeric: exit 2 |
| `TMPDIR` | `/tmp` | directory of the anonymous session |

### Set for children

Children are the provider and `exec` programs. Both variables exist so
that a nested kernel picks up where its parent is; everything else passes
through unchanged.

| Variable | Value |
|---|---|
| `AKERN_MODEL` | current `provider/model`; follows `set` |
| `AKERN_RECURSION_LIMIT` | the value read, minus 1 |

## 2. Arguments

```
akern-loop [-p] [-s session.jsonl] [-m provider/model] [-S system.md] [-P key=value]...
           [--max-processes N] [--kill-grace SECONDS] [--max-line-bytes N] [prompt...]
```

Flags are parsed with `getopt_long`: `--flag value` or `--flag=value`, and
parsing stops at the first word that is not a flag. An unknown flag, a missing
or bad value, or a `-P` without `key=`: usage on stderr, exit 2.

| Argument | Default | Meaning |
|---|---|---|
| `-p`, `--print` | off | print mode: stdout per [§4](#print-mode); exits after it has run once and is quiescent, without waiting for EOF |
| `-s`, `--session path` | `$TMPDIR/akern-session-XXXXXX` | session file. Created if missing; locked through `path.owner`. Owned by another kernel: exit 2 |
| `-m`, `--model provider/model` | `$AKERN_MODEL` | runs `$AKERN_HOME/providers/<provider>`; the request's `model` is the part after `/` |
| `-S`, `--system path` | `$AKERN_HOME/system.md` | system prompt file, read again for every request. Unreadable or missing: `system` is `null` |
| `-P`, `--param key=value` | | repeatable. A provider param in the request's `params`. `value` is sent as JSON when it is a JSON value (`max_tokens=64` is the number 64, `thinking={"type":"adaptive"}` an object), else as a string (`effort=high`) |
| `prompt...` | | words joined by spaces, queued as `source: prompt`, `when: idle`, before stdin is read |

### Limit flags

| Flag | Default | Value |
|---|---|---|
| `--max-processes N` | `32` | `exec` programs running at once; a whole number, at least 1 |
| `--kill-grace SECONDS` | `2` | from `SIGTERM` to `SIGKILL`; a number, at least 0 |
| `--max-line-bytes N` | `67108864` | longest stdin line ([§3](#3-stdin)) or provider line (a longer one is dropped whole and the provider killed); an `exec` program whose kept output would exceed it is killed. A whole number, at least 1 |

Unwritten stdout is capped at 8 MiB: past that, provider and tool stdout stop
being read until it drains. If stdout closes, events are discarded and the
kernel carries on.

### Exit

The kernel exits when it is quiescent and either stdin is at EOF or `-p` is
set and a run has started. Quiescent means: no run, no pending abort, no
`now`, `turn` or `idle` item queued, and no running job. A finished job's
result is a `turn` item, so it keeps the kernel alive until it is delivered;
only results deferred by `abort` wait for the next delivery or exit. So
`echo "start the server and run the e2e tests" | akern-loop` waits for the
tests, and a running job (a dev server included) keeps it alive: a front end
that wants jobs killed at EOF sends `{"type":"abort","jobs":true}` first. `-p`
does not wait for EOF, which lets a sub-agent keep an open stdin pipe and still
finish.

Shutdown, whatever its cause: stdin is no longer read; an open run ends
`aborted` as by `abort`, except that a response being streamed is discarded;
every worker's process group gets `SIGTERM`, then `SIGKILL` after
`--kill-grace` ([kernel.md: proc](kernel.md#proc)). Once all have exited, one
last user message closes every open `tool_pending` with its `job_result` (a
call that never started: `Not run: the kernel stopped.`, `ran` 0), stdout is
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
| `2` | bad usage, no model, recursion limit, session cannot be opened or is owned, fatal system error. `akern-loop: <reason>` on stderr |
| `130` | shutdown by signal |

## 3. Stdin

Lines end with `\n`; a final unterminated line is read at EOF. Each line is
trimmed and empty lines are ignored. A line starting with `{` is a command,
anything else is plain text. Commands act as soon as they are read, whatever
the run is doing. A line longer than `--max-line-bytes` is dropped whole, through its
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
| `when` | `now` \| `turn` \| `idle` \| `deferred` | no | `turn` | [delivery mode](kernel.md#delivery-modes); anything else: error |
| `source` | `prompt` \| `inject` | no | `prompt` | reported in `queued`; anything else: error |

Reply `ok`, then a `queued` event.

### prompt

`send` with `when: "idle"` (a `when` field is ignored). The fields are the same.

### steer

`send` with `when: "turn"` (a `when` field is ignored). The fields are the same.

### abort

End the current run. The provider gets `SIGTERM`. The turn's unfinished calls
are killed (`Killed: the run was aborted.`) or, if not yet started, cancelled
(`Not run: the run was aborted.`); its `wait: false` calls are answered
pending. Jobs keep running. Queued `turn` and `idle` user items (`prompt` and
`inject`) are handed back in `run_end.returned`; queued `now` items stay and
start the next run; queued job results stay and become `deferred`, so they
start no run but go with the next delivery, or close their calls at exit. When
no run is open, only `jobs` has an effect.

```json
{"type":"abort","jobs":true}
```

| Field | Type | Required | Default | Meaning |
|---|---|---|---|---|
| `jobs` | bool | no | `false` | also kill every running job, `wait: false` calls of the turn included |

Reply `ok`.

### kill

Kill one job's process group (`SIGTERM`, then `SIGKILL` after `--kill-grace`). Its
result is still delivered, with `"killed":true`. A job still waiting for a
`--max-processes` slot never starts; its result is `Not run: killed before it started.`

```json
{"type":"kill","job":"tu_07"}
```

| Field | Type | Required | Meaning |
|---|---|---|---|
| `job` | string | yes | the call id of a pending or `wait: false` call |

Reply `ok`, or error `no such job` when no live job has that id: the id is
unknown, or the job's result is already queued or delivered. (A `wait: false`
call that has finished while its turn is still unanswered gets `ok`, with no
effect.)

### set

Change the request settings from the next request on. Nothing restarts, so jobs survive.

```json
{"type":"set","model":"anthropic/claude-opus-5-5","system":"/p/system.md","params":{"max_tokens":64000}}
```

| Field | Type | Required | Default | Meaning |
|---|---|---|---|---|
| `model` | string | no | unchanged | `provider/model`; also exported as `AKERN_MODEL` |
| `system` | string | no | unchanged | path of the system prompt file |
| `params` | object | no | unchanged | replaces all provider params, `-P` ones included; values keep their JSON types, as with `-P` |

A `model` or `system` that is not a string, or `params` that is not an object,
is ignored. Reply `ok`.

### append

Append a session entry for someone else.

```json
{"type":"append","entry":{"type":"x-note","text":"checkpoint"},"ref":"a"}
```

| Field | Type | Required | Default | Meaning |
|---|---|---|---|---|
| `entry` | object | yes | | the entry. `id` and `ts` are assigned by the kernel |
| `entry.type` | string | yes | | entry type |
| `entry.parent` | `null` \| string | no | absent | absent: append at the head now. `null`: a new root. An id: must exist. With `parent`, the append is held until the driver is idle or a turn ends without tool calls, so a tool result never lands on a chain without its call |

Reply `ok` with `id`, only when `ref` is given, once the entry is appended.
Error, at once: `entry needs a type, and parent must be null or an existing id`.

### status

Ask for the driver state. It has no fields. Reply: a `status` event, sent even without `ref`.

### Replies

The `ref` is added as the last field. With `-p`, no reply is written.

| Reply | Shape | When |
|---|---|---|
| `ok` | `{"type":"ok","ref":R}`; `append` adds `"id":"8 hex"` | success, **only when the command has a `ref`** |
| `status` | [status](#status-1) plus `ref` | `status` |
| `error` | `{"type":"error","where":"command","message":M,"ref":R}`, without `ref` when none | failure; nothing else changes |

| `message` | Cause |
|---|---|
| `invalid JSON` | a line starting with `{` that does not parse (never carries `ref`) |
| `line longer than --max-line-bytes` | a stdin line over `--max-line-bytes` (never carries `ref`) |
| `unknown command` | `type` missing or unrecognized |
| `content must be a string or an array of content blocks` | `send`, `prompt`, `steer` |
| `when must be now, turn, idle or deferred` | `send` |
| `source must be prompt or inject` | `send`, `prompt`, `steer` |
| `no such job` | `kill` |
| `entry needs a type, and parent must be null or an existing id` | `append` |

## 4. Stdout

One JSON object per line, each with a `type`, in the order things happen.
Each event carries `run`, `turn` or `call` where they apply. A typical
run:

```json
{"type":"hello","protocol":1,"session":"s.jsonl","head":"","model":"mock/echo"}
{"type":"queued","source":"prompt","when":"turn","n":1}
{"type":"run_start","run":1}
{"type":"message","run":1,"id":"b03a71d1","source":"inbox","message":{"role":"user","content":[{"type":"text","text":"hi"}]}}
{"type":"turn_start","run":1,"turn":1}
{"type":"text_delta","text":"echo: hi"}
{"type":"message","run":1,"id":"6690fe78","source":"assistant","message":{"role":"assistant","content":[{"type":"text","text":"echo: hi"}]}}
{"type":"turn_end","run":1,"turn":1,"usage":{"input":1,"output":1,"cache_read":0,"cache_write":0}}
{"type":"run_end","run":1,"reason":"done","returned":[],"error":null}
```

A front end that only shows the answer prints `text_delta` text as it arrives
and stops at `run_end`; a script that wants only the final text uses `-p`.

### hello

Sent once at startup, after the session is opened and before the positional prompt is queued.

| Field | Type | Meaning |
|---|---|---|
| `protocol` | int | `1` |
| `session` | string | session path, as given |
| `head` | string | head entry id; `""` for an empty session |
| `model` | string | `provider/model` |

### queued

Sent when an item enters the inbox, as it arrives; after the `ok` of the command that sent it.

| Field | Type | Meaning |
|---|---|---|
| `source` | `prompt` \| `inject` \| `job` | origin |
| `when` | `now` \| `turn` \| `idle` \| `deferred` | delivery mode |
| `n` | int | inbox size, this item included |

### run_start

Sent when a run begins, before its first message.

| Field | Type |
|---|---|
| `run` | int |

### message

Sent when a `message` entry is appended to the session.

| Field | Type | Meaning |
|---|---|---|
| `run` | int | |
| `id` | string | entry id, 8 hex |
| `source` | `assistant` \| `inbox` \| `results` \| `results+inbox` | what the message holds |
| `message` | object | the message as stored |

### replace

Sent when the kernel appends a `replace` entry for a block the API rejected
([§5](#session-file)), before the request is sent again.

| Field | Type | Meaning |
|---|---|---|
| `run` | int | |
| `id` | string | the `replace` entry's id |
| `target` | string | the replaced `message` entry's id |
| `message` | object | the message as it is sent from now on |

### turn_start

Sent when the provider is started with the request.

| Field | Type |
|---|---|
| `run` | int |
| `turn` | int |

### Provider events

Every other valid JSON line the provider writes, forwarded verbatim
([providers.md §3](providers.md#3-output)). They have no `run` or `turn` and
belong to the open turn: `text_delta`, `thinking_delta`, `tool_use_start`,
`tool_use_delta`, `x-*`. A provider `error` line is forwarded too
([error](#error)). The `result` line is not forwarded.

```json
{"type":"text_delta","text":"I'll "}
```

### tool_start

Sent when a call begins, once per `tool_use` block.

| Field | Type | Meaning |
|---|---|---|
| `run`, `turn` | int | |
| `call` | string | `tool_use` id |
| `name` | string | tool name |
| `input` | object or string | `{}` when missing or empty; a string when the model's input was not a JSON object (the call then fails) |

### tool_progress

Sent for each chunk an `exec` program writes to stderr, including after its call is answered pending. Chunks are not split on line boundaries.

| Field | Type | Meaning |
|---|---|---|
| `run` | int | the call's run |
| `call` | string | |
| `data` | string | the chunk |

### tool_end

Sent when a call's result is final. For a job, it is sent when the job finishes.

| Field | Type | Meaning |
|---|---|---|
| `run` | int | the call's run |
| `call` | string | |
| `is_error` | bool | |
| `status` | `done` \| `killed` \| `cancelled` \| `not run` | `cancelled`: not started before an abort; `not run`: a job killed before it started, or not started when the kernel stopped |

### turn_end

Sent after the assistant message when no tools run, or after the results message. Not sent for a turn that was preempted or aborted, or whose provider failed.

| Field | Type | Meaning |
|---|---|---|
| `run`, `turn` | int | |
| `usage` | object \| null | the provider's `usage` |

### preempt

Sent when a `now` item interrupts a streaming response ([kernel.md: driver](kernel.md#states)).

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
| `error` | object \| null | with `reason: error`: kernel error `{"where","message"}`, or the provider's `error` line as sent; otherwise `null`. A provider `result` after an `error` line wins |

### status

The reply to `status`.

```json
{"type":"status","state":"waiting","run":1,"turn":2,"model":"mock/echo","head":"699d41fa","inbox":0,
 "calls":[{"call":"tu_2","name":"exec","state":"running"}],
 "jobs":[{"call":"tu_1","name":"exec","input":{"argv":["make"],"wait":false},"pid":27798,"state":"running"}]}
```

| Field | Type | Meaning |
|---|---|---|
| `state` | `idle` \| `stream` \| `aborting` \| `waiting` | driver state; `waiting`: on this turn's calls |
| `run`, `turn` | int | latest; `0` before the first |
| `model` | string | |
| `head` | string | |
| `inbox` | int | queued items |
| `calls` | array | this turn's calls, until they are answered: `{"call","name","state"}`, `state` one of `waiting`, `running`, `done`, `cancelled` |
| `jobs` | array | live jobs (pending and `wait: false` calls): `{"call","name","input","pid","state"}`; `pid` is `0` when no process runs |
| `ref` | any | when given |

### ok

The reply to a successful command that carries a `ref` ([§3](#replies)).

| Field | Type | Meaning |
|---|---|---|
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
| `provider` | `no provider for <model> in <home>/providers` | run ends `error` |
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

With `-p` nothing above is written. Instead:

| Stream | Content |
|---|---|
| stdout | at each run end, the text of the run's last assistant message (text blocks joined) and `\n`; nothing when empty |
| stderr | at a run end with `reason: error`: `akern-loop: <error.message>`; startup failures as in [§2](#exit) |

## 5. Files

### Home

`$AKERN_HOME` is read, never written:

```
providers/<name>                   the program -m <name>/… starts (providers.md)
system.md                          the system prompt without -S; `make` and `make install`
                                   write it, listing each skills/<name>/SKILL.md (tools.md §3)
```

### Layout

Everything a session produces sits next to its file, named after it:

```
S                                  the log: one JSON entry per line, append-only
S.owner                            lock; {"pid":N} while a kernel holds S, empty after
S.d/calls/<call>.out|.err          an exec stream past its limit (stdout_limit, stderr_limit),
                                   or both streams of a call answered pending
```

| Path | Created | Removed |
|---|---|---|
| `S` | at open, if missing (mode 0644; the anonymous session is created by `mkstemp`, mode 0600), with a `session` header line | never |
| `S.owner` | at open | never; emptied at exit |
| `S.d/` and its subdirectories | on first use | never |
| `calls/` files | when a stream spills, or once a call answered pending has a process | never |

A `calls/` file that cannot be created or written is not written further; the
call's result adds `stdout_file_error` (or `stderr_file_error`) with the
system's reason, and the file keeps only what was written before.

`<call>` is the `tool_use` id with every character but letters, digits and `-`
replaced by `_`. The anonymous session (no `-s`) is
`$TMPDIR/akern-session-XXXXXX`, and its `.owner` and `.d` sit beside it in
`$TMPDIR`.

### Session file

Line 1 is a header; every later line is an entry.

```json
{"type":"session","v":1,"id":"5f2c9a1e","cwd":"/home/k/proj","created":1763470000}
{"type":"message","id":"a1b2c3d4","parent":null,"ts":1763470001,"message":{"role":"user","content":[…]}}
{"type":"message","id":"e5f6a7b8","parent":"a1b2c3d4","ts":1763470009,"message":{"role":"assistant","content":[…]},
 "meta":{"model":"anthropic/claude-opus-5-5","stop":"tool_use","run":1,"turn":1,
         "usage":{"input":812,"output":64,"cache_read":0,"cache_write":812}}}
```

| Field | Required | Meaning |
|---|---|---|
| `type` | yes | `message`, or anything else |
| `id` | yes | unique within the file |
| `parent` | yes | the `id` of the previous entry in this chain, or `null` for a root |
| `ts` | yes | when it was appended |
| `message` | for `message` | a message ([providers.md §5](providers.md#5-messages)), stored verbatim |
| `meta` | no | facts not sent to the model; the kernel writes `{model, stop, run, turn, usage}` on assistant messages |

| Rule | |
|---|---|
| head | the last entry in the file; reported as `hello.head` |
| context | follow `parent` from the head to a root, reverse, keep the `message` of each `message` entry, or of the last `replace` entry in the file that targets it |
| other types | take part in the chain but add nothing to the context; that is how extensions keep state beside the conversation |

A `replace` entry changes what a message entry sends from then on, without
rewriting the file:

```json
{"type":"replace","id":"c9d0e1f2","parent":"e5f6a7b8","ts":1763470012,"target":"a1b2c3d4",
 "message":{"role":"user","content":[{"type":"text","text":"look"},{"type":"text","text":"[image omitted: image exceeds 5 MB maximum]"}]}}
```

| Field | Meaning |
|---|---|
| `target` | the `id` of a `message` entry; anything else, or an id not yet in the file, is ignored |
| `message` | the whole message to send in its place: an object; anything else is ignored |

Like any entry it is appended at the head and moves it, but its effect is not
tied to its chain: the last one in the file for a target wins on every branch,
after a resume and in a copied file. The kernel appends one when a provider
names a block the API rejected ([kernel.md: driver](kernel.md#states)); a front
end appends one with `append`, to redact a secret for example. The original
message stays in the file.

Conventional entry types, written by commands and front ends; the kernel
treats them like any other:

| Type | Meaning |
|---|---|
| `branch` | a marker whose `parent` is the entry to continue from; moving the head is appending one |
| `name` | `{"name":"…"}`, the session's display name |
| `label` | `{"target":"<id>","label":"…"}`, a bookmark on an entry |
| `x-*` | extension state |

Compaction is not an entry type: append a `message` entry with
`parent: null` whose message is the summary, then re-append the entries to keep
as its descendants (`meta.copy_of` records the original id). The old chain
stays in the file. Copies drop blocks that are only valid in the conversation
that produced them (thinking blocks, on providers that bind them); the provider
documents which.

### Writing

| Session | Who appends |
|---|---|
| owned (a kernel holds a write lock on `S.owner`) | only that kernel. Whoever drives it sends `append` on its stdin ([§3](#append)) |
| not owned | anyone: one `write(2)` of one complete line under an exclusive `flock` on `S`, using the last entry's `id`, read under the lock, as `parent` |

Never rewrite or truncate: append-only is what makes concurrent readers safe
and prompt caches stable.

### Session lifecycle

| Step | What happens |
|---|---|
| open | create `S` if missing; take the lock on `S.owner` (held by another kernel: exit 2, naming its pid); write `{"pid":N}` into it; read every entry; write the header if `S` was empty |
| run | every message and `append` is one line appended to `S`; the head moves to it |
| exit | `S.owner` is emptied; the lock goes with the process |
| crash | the lock goes with the process; `S.owner` keeps a dead pid, which the next open overwrites |
| resume | `akern-loop -s S`: the context is rebuilt from the head |

A `tool_pending` with no later `job_result` was running when its kernel was
killed with `SIGKILL`; any other exit closes it. Its files keep whatever it
wrote.

### Branches and forks

| Operation | How |
|---|---|
| branch in place | `{"type":"append","entry":{"type":"branch","parent":"<id>"}}`: the new entry is the head, so the next request continues from `<id>` |
| new root | `"parent": null`: a context with no history (compaction re-appends a summary under it) |
| fork | copy the file: `cp S F && akern-loop -s F`. The copy shares history and then diverges. Paths in its messages (output files) still point at `S.d/` |

An `append` with `parent` is held while a run is busy ([§3](#append)).

### Sub-agents

A sub-agent is `akern-loop -p` on a child session, run by an `exec` call
exactly as a person would run it; there is no wrapper command. The parent
names its session with `-s`; without one, it is anonymous. Its files follow the
same layout, next to that path:

```
s.jsonl                          parent; the call's tool_use holds the child's argv
s.jsonl.d/calls/tu_1.out         the sub-agent's -p answer, as the job's stdout
agents/rename.jsonl              the sub-agent's whole conversation (-s agents/rename.jsonl)
agents/rename.jsonl.d/…          its own spills and jobs
```

| Step | What happens |
|---|---|
| start | `exec {"argv":["akern-loop","-p","-s","agents/rename.jsonl","…"],"wait":false,"stdin_open":true}`; the directory must exist. `wait: false` lets the parent keep working; without it, the parent waits for the answer until a message arrives |
| model | the parent's: `AKERN_MODEL` is inherited, unless the argv passes `-m` |
| run | the child appends to its own file only; the parent sees it as a job (`tail` the child's file to watch) |
| steer | `write {"job":"<call id>","content":"…\n"}` to the child's stdin: a plain line is a `turn` item for it ([delivery modes](kernel.md#delivery-modes)), `{"type":"send","when":"now","content":"…"}` interrupts it. Each level steers the one below the same way |
| end | the child exits once quiescent (`-p` does not wait for EOF); its final answer is the job's stdout, in its `job_result`. Messages up from a child mid-run are not supported: it reports once, at the end |
| resume | the same `-s` again continues the child's conversation |

Recursion is bounded by `AKERN_RECURSION_LIMIT` ([§1](#1-environment-variables)),
which every nested kernel inherits however it is reached (directly, through
`sh -c`, a script, a shim); nobody inspects `exec` calls. With the default 4, a
top-level kernel can have three levels of sub-agents below it. Each
sub-agent's `tool_use` holds its argv, `-s` included, so a front end can walk
the tree from session to session. Two kernels given the same `-s` at once: the
second exits 2.
