# 02 — Protocols

These contracts are the design. Anything that honors them is a valid part of
the system, including a replacement kernel. The behavior they encode
(delivery modes, preemption, jobs) is defined in
[concurrency.md](concurrency.md). Protocol version: **1** (draft).

## 0. Conventions

- Every stream is **JSON lines**: one complete JSON object per line, UTF-8,
  terminated by `\n`. No pretty-printing on the wire.
- Every object on a stream has a string `type`.
- **Unknown types and unknown fields must be ignored**, by everyone. This is
  the forward-compatibility rule; extensions rely on it to add their own.
- Extension-defined types and fields are prefixed `x-` so they never collide
  with future core names.
- Paths are absolute. Times are Unix seconds.
- Ids: entry ids are 8 hex chars; `run` and `turn` are integers counting from 1;
  `call` is the provider's `tool_use` id. A job has no id of its own: it is
  named by its call id, which the model already knows.

## 1. Messages

The canonical message shape is the Anthropic Messages shape: a `role` and an
array of typed content blocks. It is the most expressive of the common formats,
so other providers translate down from it rather than the kernel translating up.

```json
{"role":"user","content":[{"type":"text","text":"list the files"}]}

{"role":"assistant","content":[
  {"type":"thinking","thinking":"","signature":"..."},
  {"type":"text","text":"I'll look."},
  {"type":"tool_use","id":"tu_01","name":"exec","input":{"argv":["ls"]}},
  {"type":"tool_use","id":"tu_02","name":"read","input":{"path":"/p/README.md"}}]}

{"role":"user","content":[
  {"type":"tool_pending","tool_use_id":"tu_01","name":"exec","state":"running","pid":4242,
   "out":"/p/s.jsonl.d/calls/tu_01.out","err":"/p/s.jsonl.d/calls/tu_01.err","stdin_open":false},
  {"type":"tool_result","tool_use_id":"tu_02","is_error":false,
   "content":[{"type":"text","text":"# agent\n…"}]},
  {"type":"text","text":"use the staging config"}]}

{"role":"user","content":[
  {"type":"job_result","tool_use_id":"tu_01","name":"exec","is_error":false,"status":"exit 0","ran":41,
   "content":[{"type":"text","text":"{\"stdout\":\"…\",\"stderr\":\"\",\"exit\":0}"}]}]}
```

Rules the kernel depends on:

- An assistant message's tool calls are its `tool_use` blocks, each with `id`,
  `name`, `input`. `input` is a JSON object, or a string holding the model's
  raw text when that text was not a JSON object (§4). The kernel answers such
  a call with an error `tool_result`, `input is not a JSON object: ` and the
  text, clipped to 200 bytes; the other calls of the message run as usual.
- All answers for one assistant message go back in **one** user message, one
  block per call, in call order, before any other blocks. A call that is
  finished or cancelled gets a `tool_result`. A call still in progress gets a
  `tool_pending`: `tool_use_id`, `name` and `state` (`waiting` for a
  `--max-processes` slot, or `running`), plus `pid`, `out`, `err` (its output files) and
  `stdin_open` while its process runs. It is now a job.
- Every `tool_pending` is closed by exactly one `job_result` in a later user
  message: `tool_use_id`, `name`, `is_error`, `status`, `ran` (seconds, from
  the program's start; 0 if it never started) and `content` (the result, as a `tool_result` would carry
  it). On shutdown the kernel closes every open one
  ([concurrency.md](concurrency.md#shutdown-and-end-of-input)).
- Inbox items delivered at the same moment follow as further blocks in the
  same user message. With no tool results, they form a user message of their own.
- Two user messages can follow each other (a job result delivered at exit, an
  aborted response with no complete text).
- `tool_pending` and `job_result` are facts, not API blocks: the provider
  renders them (§4).
- Everything else in a message is opaque. The kernel stores and resends the
  exact bytes the provider produced.

## 2. Session log

One file per session. Line 1 is a header; every later line is an entry.

```json
{"type":"session","v":1,"id":"5f2c9a1e","cwd":"/home/k/proj","created":1763470000}
{"type":"message","id":"a1b2c3d4","parent":null,"ts":1763470001,"message":{"role":"user","content":[...]}}
{"type":"message","id":"e5f6a7b8","parent":"a1b2c3d4","ts":1763470009,"message":{"role":"assistant","content":[...]},
 "meta":{"model":"anthropic/claude-opus-5-5","stop":"tool_use","run":1,"turn":1,
         "usage":{"input":812,"output":64,"cache_read":0,"cache_write":812}}}
```

### Entry fields

| Field | Required | Meaning |
|---|---|---|
| `type` | yes | `message`, or anything else |
| `id` | yes | unique within the file |
| `parent` | yes | `id` of the previous entry in this chain, or `null` for a root |
| `ts` | yes | when it was appended |
| `message` | for `message` | a message as in §1, stored verbatim |
| `meta` | no | facts about the entry that are not sent to the model |

### The two rules

1. **Head** = the last entry in the file.
2. **Context** = follow `parent` from the head to a root, reverse, keep the
   `message` of each `message` entry.

Entries of any other type take part in the chain but contribute nothing to
context. That is how extensions persist state alongside the conversation.

### Conventional entry types (userland, not kernel)

| Type | Written by | Meaning |
|---|---|---|
| `branch` | `akern-tree` | Marker whose `parent` is the entry to continue from; moving the head is appending one of these |
| `name` | `akern-name` | `{"name":"..."}` display name for the session |
| `label` | frontends | `{"target":"<id>","label":"..."}` bookmark on an entry |
| `x-*` | anything | extension state |

### Compaction

Not an entry type. To compact: append a `message` entry with `parent: null`
whose message is the summary, then re-append the entries to keep as its
descendants (`meta.copy_of` records the original id). The head is now on the new
chain. The old chain is still in the file.

Copies should drop provider-bound blocks that are only valid in the
conversation that produced them (thinking blocks, on providers that bind them);
the provider documents which.

### Writing

- **Ownership.** A running kernel creates `<session>.owner`, holds an
  exclusive `flock` on it for its lifetime, and writes into it `{"pid":…}`.
- **Owned session:** only the owner appends. Whoever drives the kernel sends
  `append` on its stdin (§3); nobody else can reach it.
- **Unowned session:** appends are one `write(2)` of one complete line under an
  exclusive `flock` on the session file. The appender reads the current last
  entry under the lock and uses its `id` as `parent`.
- Never rewrite or truncate. Append-only is what makes concurrent readers safe
  and prompt caches stable.

## 3. Control: commands and events

`akern-loop` has two channels: commands (and prompt lines) on stdin, events on
stdout. Whoever starts a kernel owns the conversation with it; the pipes along
the process tree are enough ([design.md](design.md#architecture) says
what that gives up).

Arguments, options, commands, events and exit status: [akern-loop.md](akern-loop.md).

A plain line on stdin is a `send`, and at end of stdin the kernel runs until
quiescent, then exits. Together they make the kernel usable with no frontend:
`echo "fix the failing test" | akern-loop -m …`, or
`akern-loop -p "fix the failing test"` for the answer alone. A person steers
their agent by typing into the terminal it runs in; a parent steers a sub-agent
with `write` to its job (§5, `stdin_open`).

A frontend that only wants the answer needs three lines of logic: print
`text_delta` text as it arrives, stop at `run_end`. A script that wants only
the final text uses `-p`.

## 4. Provider

`$AKERN_HOME/providers/<name>`. The model string `anthropic/claude-opus-5-5`
starts `providers/anthropic` and passes `claude-opus-5-5` as the model id.

**Input** (stdin, one object, then EOF):

```json
{"model":"claude-opus-5-5",
 "system":"You are ...",
 "messages":[ ... ],
 "tools":[{"name":"exec",...},{"name":"read",...},{"name":"write",...}],
 "params":{"effort":"high","max_tokens":64000}}
```

**Output** (stdout, JSON lines): zero or more streaming events, then exactly
one terminal line.

```json
{"type":"text_delta","text":"I'll "}
{"type":"thinking_delta","text":"..."}
{"type":"tool_use_start","id":"tu_01","name":"exec"}
{"type":"tool_use_delta","id":"tu_01","json":"{\"comm"}
{"type":"result","stop":"tool_use","message":{"role":"assistant","content":[...]},
 "usage":{"input":812,"output":64,"cache_read":0,"cache_write":812}}
```

or, on failure:

```json
{"type":"error","kind":"rate_limit","message":"...","status":429}
```

Rules:

- Streaming events are for display only. The kernel forwards them and keeps
  nothing. The `result` line carries the complete assistant message, and it is
  the only line the kernel parses.
- `stop` is one of `end_turn`, `tool_use`, `max_tokens`, `refusal`, `aborted`.
  The kernel runs tools **only** on `tool_use`.
- **On `SIGTERM`** (preemption or abort): within 1 second, emit `result` with
  `stop: "aborted"` and a message containing only **complete** text and
  thinking blocks that are valid to replay. **Never include `tool_use` blocks**,
  even complete ones. Drop thinking blocks that lack what the API needs to
  accept them again (a signature, for example). Then exit. An empty `content`
  array is allowed; the kernel then appends nothing. The kernel also removes
  any `tool_use` blocks an aborted result contains, in case a provider gets
  this wrong.
- `error.kind` is one of `auth`, `rate_limit`, `overloaded`,
  `context_overflow`, `invalid_request`, `network`, `other`. `context_overflow`
  matters: it is how a front end knows to compact and retry after `run_end`.
- Exit `0` if and only if a `result` line was written.
- A `tool_use` whose streamed input does not parse as a JSON object is still
  part of the `result`, with `input` as a string holding the raw text (§1);
  empty input is `{}`. It is not an `error`: the model gets the call's error
  result and can try again. When rendering history, the provider sends such
  an `input` as `{}`, since the API takes only an object there.
- The provider owns everything API-specific: credentials, retries and backoff,
  server-tool pauses, cache breakpoint placement, format translation, and
  **the API's limits**. Content the API would reject (a media type it does not
  accept, an image over its size or dimension limits) must not reach it: the
  provider replaces it with a short text note when it builds the request, and
  leaves the session untouched. Otherwise one oversized image would fail every
  later request. The kernel keeps no list of media types; the provider does.
- **Rendering.** The provider turns `tool_pending` and `job_result` (§1) into
  what its API accepts, and merges consecutive user messages if its API needs
  that. Rendering depends only on each message, so earlier messages render the
  same on every request and the prompt cache holds (media limits that count
  the whole request aside).
- `providers/anthropic` answers a `tool_pending` with a `tool_result` saying
  where the process and its output are (`Still running in the background (pid
  N, its own process group); its output so far is in OUT and ERR. Its result
  arrives later as a message.`, plus how to write to its stdin when it is
  open; any other state: `Not finished yet; its result arrives later as a
  message.`). A `job_result` becomes a text block, `<job call=… name=…
  status=… ran="Ns">`, the result's text, `</job>`, with the result's
  non-text blocks beside it. In a merged user message the `tool_result` blocks
  come first. Its list of accepted media types is `ACCEPTED_MEDIA`; any other
  becomes `[image omitted: image/heic is not a type the API accepts. The
  original is still in the session.]`.
- `providers/claude-code` runs each request as one turn of the `claude` CLI
  (`claude -p`), on the user's Claude Code subscription; its usage counts
  against the subscription's programmatic credit. It goes through the binary
  only and never touches Claude Code's credentials. `-m claude-code/sonnet`
  passes `sonnet` to `--model` (an alias or a full model id; `--models` lists
  the aliases), `params.effort` becomes `--effort` and `params.max_tokens`
  `CLAUDE_CODE_MAX_OUTPUT_TOKENS`. Claude Code's own tools, CLAUDE.md files,
  memory, settings, skills, other MCP servers, compaction and advisor are off
  (`--tools ""`, `--setting-sources ""`, `--disable-slash-commands`,
  `--strict-mcp-config`, `CLAUDE_CODE_DISABLE_*`, `DISABLE_COMPACT`). The
  request's tools reach the model through an MCP server, the provider itself
  run as `claude-code --mcp TOOLS`, so the model calls them as
  `mcp__akern__exec` and so on. `--permission-mode dontAsk` refuses every call
  and `--max-turns 1` ends the turn after the first response, so the
  `tool_use` blocks come back unrun; the provider gives them their akern names
  again, and adds the prefix to earlier ones in the history. The history is
  rendered as `providers/anthropic` renders it, media limits included, and
  sent as `--input-format stream-json` messages, all but the last with
  `shouldQuery: false`. `claude` appends an assistant message as soon as it
  reads it but queues user messages, so the provider writes the next message
  only after `claude` acknowledges a queued one with a `result` line.
  `--include-partial-messages` carries the API's stream events, which map to
  `text_delta`, `tool_use_*`, usage and `stop` as in `providers/anthropic`; a
  failed turn's `api_error_status` and text pick the `error.kind`.
  What it loses against `providers/anthropic`: the system prompt is not the
  request's alone (Claude Code adds an identity line, an environment block and
  context reminders such as the date and the account's email); the prompt
  cache holds only the system prompt and tools, since those reminders sit at
  the end of each request; thinking arrives without its text, so nothing shows
  as `thinking_delta` (the signed blocks are still kept and replayed); there
  are no server-side fallbacks, no `cache_control`, and no retries but
  Claude Code's own; the model knows the tools by their `mcp__akern__` names;
  and each request starts a `claude` process, which takes a second or two
  before the model is called.
- An API with asynchronous tool results, such as a future OpenAI provider,
  could send a `job_result` as a native `function_call_output` instead.
- `<provider> --models` prints one JSON line per model:
  `{"id":"...","context":1000000,"max_output":128000}`.

Because a provider is just a filter, providers compose: a `providers/logged`
script can `tee` the request and exec another provider; a router can pick a
model per request.

## 5. Tools: exec, read, write

Like an OS kernel's core syscalls, the agent kernel gives the model `exec`,
`read` and `write`, over files and jobs. Their definitions are compiled into
the kernel and sent to the provider as `[exec, read, write]`; they are what
the model is told, and there is no tool registry and no `--describe`.
Everything else the model can do is a program it runs with `exec`, explained
by a skill (§8).

```json
{"name":"exec","description":"Run a program. … The result is a JSON object with stdout, stderr and exit …",
 "input_schema":{"type":"object","properties":{
   "argv":{"type":"array","items":{"type":"string"},"minItems":1},
   "stdin":{"type":"string"},
   "stdin_open":{"type":"boolean"},
   "wait":{"type":"boolean"},
   "stdout_type":{"type":"string"},
   "stdout_limit":{"type":"integer","minimum":0},
   "stderr_limit":{"type":"integer","minimum":0}},"required":["argv"]}}
```

A call to any other name gets an error result (`Unknown tool: X. The tools are
exec, read and write.`), and so does a call whose `input` is not a JSON object
(§1). `read` and
`write` are [below](#read-and-write).

### exec input

| Field | Meaning |
|---|---|
| `argv` | the program and its arguments. `argv[0]` is looked up on `PATH` and run with `execvp`. There is no shell, so no expansion or word splitting; the model runs `["sh","-c","…"]` when it wants one |
| `stdin` | optional. Text written to the program's stdin, which is then closed (unless `stdin_open`). Nothing passes through a shell, so it needs no escaping, and it has no argv size limit. For programs that read input: `python3 -`, `patch`, `psql`. Files are written with `write`, not through a program |
| `stdin_open` | optional boolean. The kernel does not close the program's stdin pipe after writing the `stdin` text (if any), so the two combine. The model sends more with `write {"job":"<call id>","content":"…"}`; `close: true` ends input. For sub-agents and interactive background programs (`python3 -i`, `psql`, `gdb`) read through their `.out` file. The pipe is an ordinary anonymous one the OS frees when both ends close, with nothing to clean up; it is the handle POSIX gives the parent, which is why the kernel writes for the model rather than exposing a path (`/proc/PID/fd/0` would be Linux-only and subject to pid reuse) |
| `wait` | optional boolean. `false`: the model does not wait for the program (a server, a watcher, a sub-agent it will steer). The turn goes on once it has started, and it is answered with a `tool_pending`. Absent or `true`: the turn waits for it, until a message arrives ([concurrency.md](concurrency.md#calls-and-jobs)) |
| `stdout_type` | optional. A MIME type: stdout is returned as an image block (`image/*`) or a document block (anything else) instead of text. For programs that generate media; a media file is `read` with `type`. The kernel checks no list; the provider replaces a type its API does not accept (§4) |
| `stdout_limit` | optional. Bytes of stdout kept in the result; default 100000. Longer output is spilled to a file (below). Does not apply when `stdout_type` is set; the provider guards media size |
| `stderr_limit` | optional. The same for stderr; default 100000 |

### The exec process

| Channel | Carries |
|---|---|
| stdin | a pipe: the call's `stdin` text, then EOF; empty (EOF at once) without it; with `stdin_open`, EOF only after a `write` with `close: true` |
| stdout, stderr | two pipes, collected separately and in full; past `stdout_limit` or `stderr_limit` the rest goes to a file |
| exit status | reported as-is; anything but `0` sets `is_error` |
| process group | its own, so cancellation reaches everything it started |
| env | §7 |
| cwd | the kernel's working directory |

Nothing else passes between the program and the kernel. A command cannot
change how its call is handled; the model decides that with `wait`.

**Result.** One `text` block holding a JSON object, so nothing is lost or
interleaved:

```json
{"stdout":"…","stderr":"…","exit":0}
```

**Spilling.** When a stream is longer than its limit, the result keeps
its first that-many bytes (a character split by the cut shows as U+FFFD), the whole
stream is saved to `<session>.d/calls/<call id>.out` (or `.err`), and the
result adds the total size and the path. Nothing is lost; the model reads the
rest with `read` (from an `offset`), `grep` or `tail`, or reruns with a higher limit:

```json
{"stdout":"…first 100000 bytes…","stdout_total":4812333,
 "stdout_file":"/…/s.jsonl.d/calls/tu_07.out","stderr":"","exit":0}
```

`stderr_total` and `stderr_file` work the same way. If the file cannot be
created or written, the kernel stops writing it and the result adds
`stdout_file_error` (or `stderr_file_error`): the system's reason, such as
`"No space left on device"`. The file then holds only what was written before.

With `stdout_type`, a successful run returns the image or document block,
followed by a text block with `{"stderr", "exit"}`. A failed run returns the
usual JSON object, with a note in place of the binary stdout.

`exit` becomes `signal` when the program was killed by a signal, and
`"killed":true` is added when the kernel killed it (the `kill` command,
`abort` with `jobs: true`, or `--max-line-bytes`). A job the model
stops itself with `kill -TERM -- -PID` shows up as `"signal":15`. Output is
UTF-8 text; invalid bytes become U+FFFD.

**Pending.** A call answered with a `tool_pending` (§1) is a job, named by its
call id. From then on both streams are also written whole to
`<session>.d/calls/<call id>.out` and `.err` (a stream that had already
spilled keeps its spill file), and the `tool_pending` names them with the pid,
so the model can `tail` them and `kill` the job's process group. A call that
was still waiting for a `--max-processes` slot gets its files when it starts; killed
before then, it never starts, and its result is `Not run: killed before it started.`

When the program exits, the same JSON object is the job's result: it is
delivered as a `job_result` through the inbox ([concurrency.md](concurrency.md#calls-and-jobs)). stderr is also
forwarded live as `tool_progress` events for frontends.

**Cancellation.** `SIGTERM` to the process group, then `SIGKILL` after
`--kill-grace`.

### read and write

```json
{"name":"read","input_schema":{"type":"object","properties":{
   "path":{"type":"string"},
   "offset":{"type":"integer","minimum":1},
   "limit":{"type":"integer","minimum":1},
   "type":{"type":"string"}},"required":["path"]}}
{"name":"write","input_schema":{"type":"object","properties":{
   "path":{"type":"string"},
   "job":{"type":"string"},
   "content":{"type":"string"},
   "append":{"type":"boolean"},
   "close":{"type":"boolean"}},"required":["content"]}}
```

Both run inside the kernel and finish at once: no process, no worker slot.

**`read {path, offset?, limit?, type?}`** returns the file's exact text: 2000 lines
from `offset` (a 1-based line number, default 1), or `limit` lines, at most
100000 bytes. A line over 2000 characters is cut with `… (N more characters)`.
If more remains, the text ends with a note line
`[lines A-B of N; read on with offset C]`. With `type`, a MIME type, the whole
file comes back as an image block (`image/*`) or a document block (anything
else), up to 32 MB (`PATH: N bytes is too large to view`); the kernel does not
look at its bytes. Without `type`, a file with a NUL byte in its first 8 KB is
refused (`PATH is a binary file (N bytes)`), as is a directory
(`PATH is a directory`). Errors
from built-in tools state what is wrong, never what to do about it: the model
knows the remedy, and advice belongs in skills.

**`write {path, content, append?}`** replaces the file atomically (a temp file,
then `rename`; an existing file keeps its mode, a new one gets `0644`), or
appends with `append: true`. The directory must exist (`PATH: directory does
not exist`). It replies `Created PATH (N bytes)`,
`Replaced PATH (N bytes)` or `Appended N bytes to PATH`.

**`write {job, content, close?}`** appends `content` to a running job's stdin
pipe, which the kernel holds (`stdin_open`, above); `job` is the call id of a
pending or `wait: false` call. `close: true` then closes the pipe, so the
program sees EOF. It replies `Wrote N bytes to the stdin of ID.` (`, then
closed it` with `close`), or an error: `"ID" is not a running job.`,
`The stdin of ID is closed.`

## 6. Interception

The kernel has no hook points. Requests are changed by a wrapping provider,
programs by a shim earlier on `PATH`, prompts and runs by the front end:
[design.md](design.md#no-interception-points).

### Asking the user

A shim or tool that needs an answer from the person calls the program named by
`AKERN_ASK`, the same way `ssh` calls `SSH_ASKPASS`:

```sh
"$AKERN_ASK" confirm "Run: rm -rf build?"   # exit 0 = yes, 1 = no
"$AKERN_ASK" input   "Commit message:"      # answer on stdout
"$AKERN_ASK" select  "Pick one:" a b c      # choice on stdout
```

The default implementation talks to `/dev/tty` and holds a lock while it does,
so questions from parallel calls are asked one at a time. A frontend that owns
the terminal exports its own helper, which reaches the frontend however the
frontend chooses and renders the question natively. The kernel only passes the variable down. If
`AKERN_ASK` is unset, there is no one to ask and a gate must deny.

## 7. Environment

Variables the kernel reads and sets for children:
[akern-loop.md §1](akern-loop.md#1-environment-variables).

## 8. Commands and skills

Not a kernel protocol, but conventions worth fixing early so the ecosystem is
uniform. A **command** plus a **skill** is how the system is extended
([principle 11](design.md#principles)): the command is any program on
`PATH`, and the skill tells the model when and how to call it. The model
reaches commands through `exec`, exactly as a person does.

### Skills

`$AKERN_HOME/skills/<name>/SKILL.md`, a Markdown file with a short header:

```markdown
---
name: akern
description: Background jobs and sub-agents — stop, wait for or message jobs, …
---

# Jobs and sub-agents
…which argv does what, and when to use it…
```

`make install` lists every skill in `$AKERN_HOME/system.md`, the system prompt
when `akern-loop` has no `-S` ([akern-loop.md §5](akern-loop.md#home)). The model reads a skill's file
(`read`) before relying on it, so only the one-line list costs
context on every request. A skill can cover any program, not only `akern-*`
commands: `gh`, `psql`, an MCP bridge, a project's own scripts.

### Commands

- Commands are run by their own names; there is no dispatcher, since `PATH`
  already is one.
- `akern-<cmd> --help` prints one line: usage, then what it does. There is no
  listing command: the skills are the model's index, and people tab-complete
  `akern-`.
- Shipped: `akern-edit` (self-contained C, exact-match replacement), with the
  `files` skill, which covers it, editing tips and images; reading and writing
  files are the `read` and `write` tools. The `akern` skill covers jobs,
  `stdin_open` and sub-agents as `akern-loop -p`; it needs no command of its own.
  There are no wrapper commands: when one seems needed, the API it would wrap
  is missing something, and that is fixed instead. Listing, stopping and waiting
  for jobs need no command: a pending call's answer names its pid and files,
  `kill -TERM -- -PID` stops one, and
  ending the turn waits for its result.
- There is no write command: files are written with the `write` tool, so
  their contents never go inside a shell script and the shell's escape traps
  (heredocs expanding `$` and backticks, `echo` reading backslashes, `printf`
  reading `%`) cannot occur. The model runs `mkdir -p` first for a new
  directory.
- A command that operates on a session takes the path as its first argument.
  It writes to the file only when the session is not
  owned (§2); an owned session is changed through its kernel's stdin, by
  whoever drives it.
- Interactive frontends map `/cmd args` to the same lookup with the session
  path as the first argument, and show the command's stdout. The shell and the prompt line share one
  namespace.
- `akern "some text"` runs
  `akern-loop -- "$*" | jq -n --unbuffered -rj -f "$AKERN_HOME/render.jq"` with
  stdin still the terminal: after the first prompt every line typed is sent
  too, steering the agent if it is working (§3), and Ctrl-D ends. `akern`
  alone is the same without a first prompt. Whenever the state changes,
  render.jq prints a dim status line: `… working` while a run is active,
  `⏵ N jobs running · waiting for input` while `tool_pending` calls have no
  `job_result` yet, `› waiting for input` otherwise. Scripts that want one answer use
  `akern-loop -p "…"`. The renderer is data: edit `render.jq` to change what
  is shown.
