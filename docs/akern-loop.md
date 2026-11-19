# akern-loop

The kernel as a command: what it reads, what it writes. Behavior (delivery
modes, preemption, jobs) is defined in [concurrency.md](concurrency.md); the
wire contracts in [protocols.md](protocols.md). Tool inputs (`exec`, `read`,
`write`) are model-facing, not CLI: [protocols.md §5](protocols.md#5-tools-exec-read-write).

## 1. Environment variables

### Read

| Variable | Default | Meaning |
|---|---|---|
| `AKERN_HOME` | `$HOME/.akern` (`./.akern` without `HOME`) | holds `providers/<name>` and `system.md` |
| `AKERN_MODEL` | none | `provider/model` when `-m` is absent. Neither given: exit 2 |
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
being read until it drains.

### Exit

The kernel exits when it is quiescent and either stdin is at EOF or `-p` is
set and a run has started. Quiescent means: no run, no pending abort, no inbox
item due, and no running job. A finished job's result is a `turn` item, so it
keeps the kernel alive until it is delivered; only results deferred by `abort`
wait for the next delivery or exit. A front end that wants jobs killed at EOF
sends `{"type":"abort","jobs":true}` first. On exit, remaining workers get
`SIGTERM`, then `SIGKILL` after `--kill-grace`, and stdout is flushed.

| Signal | Effect |
|---|---|
| `SIGINT` | `abort` (jobs kept), [interrupt](#interrupt) |
| second `SIGINT` within 3 s, `SIGTERM`, `SIGHUP` | shutdown: an open run ends `aborted` as by `abort`, its calls answered; a last message closes every pending call ([concurrency.md](concurrency.md#shutdown-and-end-of-input)); exit 130 |

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
([protocols.md §1](protocols.md#1-messages)).

### send

Queue a user item in the inbox.

```json
{"type":"send","content":"use the staging config","when":"now","ref":1}
```

| Field | Type | Required | Default | Meaning |
|---|---|---|---|---|
| `content` | string \| block array | yes | | anything else: error |
| `when` | `now` \| `turn` \| `idle` \| `deferred` | no | `turn` | delivery mode; anything else: error |
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
`inject`) are handed back in `run_end.returned`; queued job results stay and
become `deferred`, so they start no run but go with the next delivery, or close
their calls at exit. When no run is open, only `jobs` has an effect.

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

Reply `ok`, or error `no such job` when no such job is live: the id is
unknown, or the job has finished. A job with no process yet (waiting for a slot) is not affected.

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

A `model` or `system` that is not a string is ignored. Reply `ok`.

### append

Append a session entry for someone else.

```json
{"type":"append","entry":{"type":"x-note","text":"checkpoint"},"ref":"a"}
```

| Field | Type | Required | Default | Meaning |
|---|---|---|---|---|
| `entry` | object | yes | | the entry. `id` and `ts` are assigned by the kernel |
| `entry.type` | string | yes | | entry type |
| `entry.parent` | `null` \| string | no | absent | absent: append at the head now. `null`: a new root. An id: must exist. With `parent`, the append waits until no run is open or the next turn boundary |

Reply `ok` with `id`, only when `ref` is given. Error: `entry needs a type, and parent must be null or an existing id`.

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

### turn_start

Sent when the provider is started with the request.

| Field | Type |
|---|---|
| `run` | int |
| `turn` | int |

### Provider events

Every other valid JSON line the provider writes, forwarded verbatim. They have
no `run` or `turn` and belong to the open turn: `text_delta`, `thinking_delta`,
`tool_use_start`, `tool_use_delta`, `x-*`. A provider `error` line is
forwarded too ([error](#error)). The `result` line is not forwarded.

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
| `input` | object or string | `{}` when missing; a string when the model's input was not a JSON object (the call then fails) |

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

Sent after the assistant message when no tools run, or after the results message.

| Field | Type | Meaning |
|---|---|---|
| `run`, `turn` | int | |
| `usage` | object \| null | the provider's `usage` |

### preempt

Sent when a `now` item interrupts a streaming response ([concurrency.md](concurrency.md#preemption)).

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

### Print mode

With `-p` nothing above is written. Instead:

| Stream | Content |
|---|---|
| stdout | at each run end, the text of the run's last assistant message (text blocks joined) and `\n`; nothing when empty |
| stderr | at a run end with `reason: error`: `akern-loop: <error.message>`; startup failures as in [§2](#exit) |

## 5. Files

Entry format and the head/context rules: [protocols.md §2](protocols.md#2-session-log).

### Home

`$AKERN_HOME` is read, never written:

```
providers/<name>                   the program -m <name>/… starts
system.md                          the system prompt without -S; `make` and `make install`
                                   write it, listing each skills/<name>/SKILL.md
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
| `S` | at open, if missing (mode 0644), with a `session` header line | never |
| `S.owner` | at open | never; emptied at exit |
| `S.d/` and its subdirectories | on first use | never |
| `calls/` files | when a stream spills, or once a call answered pending has a process | never |

A `calls/` file that cannot be created or written is not written further; the
call's result adds `stdout_file_error` (or `stderr_file_error`) with the
system's reason, and the file keeps only what was written before.

`<call>` is the `tool_use` id with every character but letters, digits and `-`
replaced by `_`. The anonymous session (no `-s`) is
`$TMPDIR/akern-session-XXXXXX`, and its `.owner` and `.d` sit beside it in
`$TMPDIR`. Paths in events and entries are as given: a relative `-s` stays
relative.

### Session lifecycle

| Step | What happens |
|---|---|
| open | create `S` if missing; take an exclusive lock on `S.owner` (held by another kernel: exit 2); write `{"pid":N}` into it; read every entry; write the header if `S` was empty |
| head | the last entry with an `id`; reported as `hello.head` |
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

An `append` with `parent` sent while a run is open waits for the next turn
boundary.

### Sub-agents

A sub-agent is a kernel an `exec` call starts. The parent names its session
with `-s`; without one, it is anonymous. Its files follow the same layout, next
to that path:

```
s.jsonl                          parent; the call's tool_use holds the child's argv
s.jsonl.d/calls/tu_1.out         the sub-agent's -p answer, as the job's stdout
agents/rename.jsonl              the sub-agent's whole conversation (-s agents/rename.jsonl)
agents/rename.jsonl.d/…          its own spills and jobs
```

| Step | What happens |
|---|---|
| start | `exec {"argv":["akern-loop","-p","-s","agents/rename.jsonl","…"],"wait":false}`; the directory must exist |
| run | the child appends to its own file only; the parent sees it as a job (`tail` the child's file to watch) |
| steer | `write {"job":"<call id>","content":"…\n"}` to the child's stdin, with `stdin_open` |
| end | the child exits once quiescent; its final answer is the job's stdout, in its `job_result` |
| resume | the same `-s` again continues the child's conversation |

Two kernels given the same `-s` at once: the second exits 2.
