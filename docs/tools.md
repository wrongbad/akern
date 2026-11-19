# Tools

How capabilities reach the model, for `tools/` and `skills/`. The kernel gives
the model three built-in tools, `exec`, `read` and `write`; everything else is
a program on `PATH` that the model runs with `exec`, plus a skill that says
when and how. Tools don't know each other: a tool's output, errors and usage
text never name another tool, and an error states the problem, never a remedy
([design.md principle 15](design.md#principles)).

## 1. Built-in tools

Compiled into the kernel (`TOOL_SCHEMAS` in `src/calls.c`, with the
descriptions the model reads) and sent in every request as `tools`. There is
no tool registry.

```json
{"name":"exec","input_schema":{"type":"object","properties":{
   "argv":{"type":"array","items":{"type":"string"},"minItems":1},
   "stdin":{"type":"string"},
   "stdin_open":{"type":"boolean"},
   "wait":{"type":"boolean"},
   "stdout_type":{"type":"string"},
   "stdout_limit":{"type":"integer","minimum":0},
   "stderr_limit":{"type":"integer","minimum":0}},"required":["argv"]}}
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

| Call | Result |
|---|---|
| any other name | error: `Unknown tool: X. The tools are exec, read and write.` |
| `input` not a JSON object | error: `input is not a JSON object: ` and the text, clipped to 200 bytes (`…` when clipped) |

Every result is a `tool_result` (or, for a job, a later `job_result`) with a
block array as `content`; an empty text becomes `(empty)`. How calls run in
parallel, wake the turn and become jobs: [kernel.md: calls](kernel.md#calls).

### exec

| Field | Meaning |
|---|---|
| `argv` | the program and its arguments, run with `execvp` (`PATH` lookup). No shell: the model runs `["sh","-c","…"]` when it wants one. Not a non-empty array of strings: error `argv must be a non-empty array of strings` |
| `stdin` | text written to the program's stdin, which is then closed (unless `stdin_open`). No escaping, no argv size limit |
| `stdin_open` | `true`: stdin stays open after the `stdin` text; the model sends more with `write {job}` and ends it with `close` |
| `wait` | `false`: the model does not wait for the program (a server, a watcher, a sub-agent it steers); the call is a job from the start. Absent or `true`: the turn waits, until a message arrives |
| `stdout_type` | a MIME type: a successful run's stdout comes back as an image block (`image/*`) or a document block (anything else). The kernel checks no list; a block the API rejects is replaced with a note in the session ([akern.md](akern.md#session-file)) |
| `stdout_limit` | bytes of stdout kept in the result; default 100000; ignored with `stdout_type` |
| `stderr_limit` | the same for stderr; default 100000 |

| Program side | |
|---|---|
| stdin | a pipe: the `stdin` text, then EOF; EOF at once without it; with `stdin_open`, EOF after a `write` with `close` |
| stdout, stderr | two pipes, collected separately and in full |
| exit status | reported as is; anything but `0` sets `is_error` |
| process group | its own, so a signal to the group reaches everything it started |
| environment, cwd | the kernel's, plus [akern.md §1](akern.md#set-for-children) |
| not found | `execvp`'s error on stderr, exit 127 |

Nothing else passes between the program and the kernel; a program cannot
change how its call is handled.

**Result.** One text block holding a JSON object:

```json
{"stdout":"…","stderr":"…","exit":0}
```

| Member | When |
|---|---|
| `exit` | the exit status; replaced by `signal` (the signal number) when a signal ended it |
| `killed: true` | the kernel killed it: `kill`, `abort` with `jobs`, or kept output over `--max-line-bytes` |
| `stdout_total`, `stdout_file` | stdout was longer than `stdout_limit`: the result keeps its first bytes (a character cut in two shows as U+FFFD) and the whole stream is in the file (likewise `stderr_…`) |
| `stdout_file_error` | the file could not be created or written: the system's reason; the file keeps only what was written before (likewise `stderr_…`) |

Output is UTF-8 text; invalid bytes become U+FFFD. With `stdout_type`, a
successful run with output returns the media block followed by a text block
`{"stderr", "exit", …}`; a failed run returns the usual object with
`stdout` replaced by `(N bytes not shown: the program failed)`.

**Jobs.** A call answered with a `tool_pending`, or started with
`wait: false`, is a job named by its call id. From the moment it is answered
pending (or starts, if it was still waiting for a slot) both streams are
written whole to `<session>.d/calls/<call>.out` and `.err` (a stream that
already spilled keeps its file), and the `tool_pending` names them with the
pid. The model watches with `tail`, stops it with `kill -TERM -- -PID`, which
shows up as `"signal":15`, and waits by ending its turn (the `job_result`
starts a new one) or by polling, `while kill -0 PID; do sleep 1; done`. stderr is also forwarded live as `tool_progress` events.

### read

`read {path, offset?, limit?, type?}`, run inside the kernel; it finishes at
once.

| Input | Result |
|---|---|
| text file | its exact text: 2000 lines from `offset` (1-based, default 1), or `limit` lines, at most 100000 bytes. A line over 2000 characters is cut with `… (N more characters)`. If more remains: a last line `[lines A-B of N; read on with offset C]` |
| `type` given | the whole file as an image block (`image/*`) or a document block with `name` the path's last component; the bytes are not inspected |

| Error | When |
|---|---|
| `path is required` | no `path` |
| `PATH: <reason>` | it cannot be opened |
| `PATH is a directory` | |
| `PATH is a binary file (N bytes)` | no `type`, and a NUL byte in its first 8 KB |
| `PATH: N bytes is too large to view` | with `type`, over 32 MB |
| `PATH has only N lines` | `offset` past the end |

### write

`write {path, content, append?}` replaces the file atomically (a temporary
`PATH.akern-PID`, then `rename`; an existing file keeps its mode, a new one
gets `0644`), or appends with `append: true`.

| Reply | When |
|---|---|
| `Created PATH (N bytes)`, `Replaced PATH (N bytes)`, `Appended N bytes to PATH` | success |
| `PATH: directory does not exist` | the directory is missing |
| `PATH: <reason>` | any other failure |
| `write needs a path or a job` | neither given |

`write {job, content, close?}` appends `content` to the stdin pipe of a job
started with `stdin_open`; `close: true` then closes it, so the program sees
EOF.

| Reply | When |
|---|---|
| `Wrote N bytes to the stdin of ID.` (`, then closed it` with `close`) | success |
| `"ID" is not a running job.` | no job with that id has a process |
| `The stdin of ID is closed.` | |

The pipe is anonymous: only the kernel holds its write end, so there is
nothing to clean up. It is the handle POSIX gives the parent, which is why the
kernel writes for the model rather than exposing a path (`/proc/PID/fd/0`
would be Linux-only and subject to pid reuse).

## 2. Programs on PATH

Any program on `PATH` is a tool: people, scripts and the model run the same
programs, the model through `exec`.

| Convention | Rule |
|---|---|
| lookup | by name; there is no dispatcher, `PATH` already is one |
| `akern-<cmd> --help` | one line: usage, then what it does. No listing command: skills are the model's index, people tab-complete `akern-` |
| no wrappers | when a wrapper seems needed, the API it would wrap is missing something, and that is fixed instead. Listing, stopping and waiting for jobs need no command (§1, Jobs) |
| no write command | files are written with `write`, so their contents never pass through a shell script and its escape traps (heredocs expanding `$` and backticks, `echo` reading backslashes, `printf` reading `%`). The model runs `mkdir -p` first for a new directory |
| session commands | take the session path as the first argument; write to the file only when it is not owned ([akern.md](akern.md#writing)); an owned session is changed through its kernel's stdin |
| slash commands | an interactive front end maps `/cmd args` to `akern-cmd SESSION args` and shows its stdout |
| shims | a program earlier on `PATH` wraps another: check, rewrite, log, redact. Shims must be reentrant, since parallel calls run them at once |

### akern-edit

`tools/akern-edit.c`, self-contained C, documented to the model by the `files`
skill.

```
akern-edit PATH OLD NEW [--all]
```

Replaces OLD with NEW byte for byte; OLD must match exactly once unless
`--all`. The file is replaced atomically, keeping its mode.

| Exit | Output |
|---|---|
| 0 | `--help` or no arguments: the usage line |
| 0 | `Edited PATH: replaced N occurrence(s). First change at line L:` and the changed lines with 3 lines of context, numbered |
| 1 | on stderr, `akern-edit: PATH: ` and one of `OLD must not be empty`, `OLD text not found`, `OLD text matches N times`, or the system's reason; wrong arguments: `akern-edit: usage: akern-edit PATH OLD NEW [--all]` |

### AKERN_ASK

A shim or tool that needs an answer from the person runs the program named by
`AKERN_ASK`, as `ssh` runs `SSH_ASKPASS`:

```sh
"$AKERN_ASK" confirm "Run: rm -rf build?"   # exit 0 = yes, 1 = no
"$AKERN_ASK" input   "Commit message:"      # answer on stdout
"$AKERN_ASK" select  "Pick one:" a b c      # choice on stdout
```

A convention, with no helper shipped yet. A helper serializes questions from
parallel calls (one that talks to `/dev/tty` holds a lock while it asks); a
front end that owns the terminal exports its own, which renders the question
natively. The kernel only passes the variable down. Unset, there is no one to
ask, and a gate must deny.

## 3. Skills

A skill tells the model when and how to run a program. It can cover any
program, not only `akern-*`: `gh`, `psql`, an MCP bridge, a project's scripts.

```markdown
---
name: akern
description: Background jobs, interactive programs and sub-agents — …
---

# Jobs and sub-agents
…which argv does what, and when…
```

| Item | Rule |
|---|---|
| location | yours in `$AKERN_HOME/skills/<name>/SKILL.md`, then the shipped ones in `$AKERN_HOME/system/skills/<name>/SKILL.md` (`skills/<name>/SKILL.md` in the repository). A directory in `skills/` hides the shipped skill of the same name, with or without a `SKILL.md` |
| header | `name:` (default: the directory name) and `description:`, one line each, in the front matter between the first two `---` lines |
| list | `Skills: read a skill's file before relying on it.`, then one line per skill: `- NAME: DESCRIPTION (absolute path of SKILL.md)`; yours, then the shipped ones, each in byte order of directory name. No skills: no list |
| use | the [prompt program](#the-prompt-program) puts the list in the system prompt; the model reads a skill's file with `read` before relying on it, so only the list costs context on every request |

| Shipped | Covers |
|---|---|
| `skills/akern` | jobs (`wait: false`, `tail`, `kill`), talking to a running program (`stdin_open`, `write {job}`), sub-agents as `akern -p` ([akern.md](akern.md#sub-agents)) |
| `skills/files` | `akern-edit`, editing tips, images and PDFs through `read`'s `type` and `exec`'s `stdout_type`, searching with ordinary programs |

### The prompt program

`tools/prompt`, `sh`, shipped as `$AKERN_HOME/system/prompt`: the kernel runs
it once at startup, and its output is the system prompt
([akern.md §2](akern.md#system-prompt)). It reads markdown and runs nothing
from the project. Its parts, in order, separated by a blank line, each ending
in a newline:

| Part | Content |
|---|---|
| your text | `$AKERN_HOME/system.md`, verbatim, when not empty |
| skills | the [list](#3-skills) |
| context files | for each directory from the repository root (the nearest ancestor of the working directory, itself included, that has a `.git`) down to the working directory, outermost first: its `AGENTS.md`, else its `CLAUDE.md`, when not empty, under a heading `# ABSOLUTE PATH`. Outside a repository, the working directory alone |

The output depends only on these files, so kernels started on the same files
send the same prompt and keep the provider's cache warm. Your own
`$AKERN_HOME/prompt` replaces this program; it can run it first
(`~/.akern/system/prompt`) and add to its output.

### Install

| `make` | `make install` (`PREFIX=/usr/local`, `AKERN_HOME=~/.akern`) |
|---|---|
| `build/bin/`: `akern`, `akern-edit`, `akern-cli`, `akern-model` | the same programs into `$PREFIX/bin` |
| `build/system/`: `providers/`, `skills/`, `render.jq`, `prompt`, `README.md` (from `home/README.md`: the layout and how to override) | copied to `$AKERN_HOME/system/`, which is removed first |

`build/` has the layout of an installed `$AKERN_HOME`, so `AKERN_HOME=build`
(what `./akern-dev.sh` sets) runs the build as if installed.
`AKERN_HOME` is the variable `akern` reads, so one exported in the shell
sets both where `make install` writes and where the programs look. An
`AKERN_HOME` that is the build directory fails `make install` and
`make uninstall`.
`$AKERN_HOME/system` is the base layer and belongs to `make install`, which
replaces it whole: a file dropped from a release does not linger. The user
layer is `$AKERN_HOME` itself, looked up first: `providers/<name>`, `render.jq`,
`prompt`, and what the shipped `prompt` reads, `system.md` and `skills/`. A
shadowing file is reported by the install. Install writes nothing there, and
removes only a copy that changes nothing: a provider, `render.jq` or `prompt`
byte-identical to the new one in `system/` is deleted and reported; one that
differs at all is kept and reported as shadowing. Neither `make` nor
`make install` writes `model` ([§4](#4-the-akern-cli-front-end)); `make clean`
removes `build/`, and with it `build/model`.

`make uninstall` removes the programs from `$PREFIX/bin` and
`$AKERN_HOME/system`, then `$AKERN_HOME` only if nothing is left in it: the user
layer, `model` and anything else of yours stay, and the uninstall says so.

## 4. The akern-cli front end

`tools/akern-cli`, a shell script for people:

```
akern-cli ["prompt text"]
```

It runs `akern [-- "$*"] | RENDER --arg prompted BOOL`, where `RENDER` is
`$AKERN_HOME/render.jq`, else `$AKERN_HOME/system/render.jq`, with stdin still the terminal: every line typed is sent, steering the agent
when it is working, and Ctrl-D ends. The shell and `jq` ignore `SIGINT`, so
Ctrl-C reaches only `akern` ([akern.md §2](akern.md#exit)).
Scripts that want one answer use `akern -p "…"`.

The model, exported as `AKERN_MODEL`, is the first that is set:

1. `AKERN_MODEL`;
2. the first line of `$AKERN_HOME/model`;
3. what `akern-model` prints, after asking on the terminal. Its failure ends
   `akern-cli`.

`tools/render.jq` is the display, as data: edit it to change what is shown.

| Event | Shown |
|---|---|
| `text_delta` | the text |
| `tool_start` | `▸ NAME INPUT`, dim |
| `message` with `tool_pending` blocks | `↳ ID keeps running` per block, dim |
| `tool_end` | `✓ CALL STATUS`, plus ` (error)`, dim |
| `preempt` | `⏸ interrupted`, yellow |
| `interrupt` | `(press Ctrl-C again within Ns to quit)`, dim |
| `notice` | `[SOURCE] MESSAGE`, yellow |
| `error` | `error (WHERE): MESSAGE`, red |
| `turn_end` | a newline |
| `run_end` not `done` | `[run N REASON]`, dim |

Whenever it changes, a dim status line follows, on a line of its own:

| Status | When |
|---|---|
| `… working` | between `run_start` and `run_end`; from the start when a prompt was given |
| `⏵ N jobs running · waiting for input` | otherwise, while some `tool_pending` has no `job_result` yet |
| `› waiting for input` | otherwise |

### akern-model

`tools/akern-model`, `sh` and `jq`. Sets the default model, the file `akern-cli`
reads.

```
akern-model [provider/model]
```

| Call | Does |
|---|---|
| `akern-model PROVIDER/MODEL` | writes it to `$AKERN_HOME/model` as one line and prints it; no terminal needed |
| `akern-model` | asks on `/dev/tty`, even when a default exists; saves and prints the choice as above |
| `akern-model --help` | the usage line |

Asking runs every executable in `$AKERN_HOME/providers`, then in
`$AKERN_HOME/system/providers` those whose name is not taken, with `--models`
([providers.md §1](providers.md#1-finding-and-running-a-provider)), one at a time, each killed after
10 s, and lists them in that order, each layer alphabetically, numbered. One that exits 0 is ready;
one that fails is marked, dim, `(unavailable: MESSAGE)`, the `message` of its
`error` line (`(unavailable)` without one). There is no table of which
variable or binary a provider needs: its error says. Choosing an unavailable
provider prints `NAME is unavailable: MESSAGE` and shows the list again. A
ready provider's ids are listed, numbered, with a last entry
`(type a model id)`; one that lists none is asked for an id directly. Prompts
and lists go to `/dev/tty`, so `akern-model`'s stdout can be captured.

```
  1) anthropic  (unavailable: ANTHROPIC_API_KEY is not set)
  2) claude-code
  3) mock
  4) openai  (unavailable: OPENAI_API_KEY is not set)
Provider [1-4]: 2
  1) fable
  2) sonnet
  3) opus
  4) haiku
  5) (type a model id)
Model [1-5]: 3
claude-code/opus
```

| Exit | Output |
|---|---|
| 0 | the saved `provider/model` on stdout |
| 1 | no terminal: on stderr, `akern-model: no model set`, or `akern-model: no terminal` when `$AKERN_HOME/model` exists. No provider: `akern-model: no provider in HOME/providers or HOME/system/providers`. None ready: the list, then `akern-model: no provider is ready`. Ctrl-D at a prompt: nothing saved |
| 2 | other arguments: the usage line on stderr |
