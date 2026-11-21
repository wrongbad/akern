# Tools

How capabilities reach the model, for `tools/` and `skills/`. The model is
given the tools in `tools.json`, `shell`, `job` and `edit`. The kernel carries
out a `shell` or `edit` call by running the program the tool's `program` names
(`bash`, `akern-edit`), and a `job` call itself; everything else is a program on `PATH`
that the model runs in the shell, plus a skill that says when and how. Tools don't know each other: a tool's output, errors and usage
text never name another tool, and an error states the problem, never a remedy
([design.md principle 15](design.md#principles)).

## 1. Built-in tools

Declared in `tools/tools.json`, with the descriptions the model reads, and
installed as `$AKERN_HOME/system/tools.json`. The kernel reads
`$AKERN_HOME/tools.json`, else `$AKERN_HOME/system/tools.json`, once at
startup. Each entry is `{name, description, input_schema, program}`: the
first three are sent in every request as `tools`, with the whitespace between
tokens dropped. `program` is what a `shell` or `edit` call runs, looked up on
`PATH` by name, so your copy can point a tool at another program (a sandbox, a
logger) as long as it takes the same arguments, or drop a tool, or reword a
description; `job` has none. `name` must be one of `shell`, `job` and `edit`,
which the kernel knows how to call; anything else stops `akern` at startup
with exit 2 ([akern.md](akern.md#tools)). There is no tool registry, and no
`read` or `write`: the model reads files with `cat`, `head`, `tail` and
`sed -n` in the shell, and writes them with `cat > path`, the text on stdin,
so it passes through no quoting (no heredoc expanding `$`, no `echo` reading
backslashes).

The schemas as shipped, without the descriptions. Every field has a
`description` too: what it means, which fields it goes with, and the
defaults, so the model has what it needs where it reads the field.

```json
{"name":"shell","program":"bash","input_schema":{"type":"object","properties":{
   "command":{"type":"string"},
   "stdin":{"type":"string"},
   "eof":{"type":"boolean"},
   "wait":{"type":"number","minimum":0},
   "stdout_name":{"type":"string"},
   "output_limit":{"type":"integer","minimum":0}},"required":["command"]}}
{"name":"job","input_schema":{"type":"object","properties":{
   "id":{"type":"string"},
   "stdin":{"type":"string"},
   "eof":{"type":"boolean"},
   "signal":{"type":"string"},
   "wait":{"type":"number","minimum":0}},"required":["id"]}}
{"name":"edit","program":"akern-edit","input_schema":{"type":"object","properties":{
   "path":{"type":"string"},
   "old":{"type":"string","minLength":1},
   "new":{"type":"string"},
   "all":{"type":"boolean"}},"required":["path","old","new"]}}
```

| Call | Runs, with `PROGRAM` the tool's `program` | stdin |
|---|---|---|
| `shell {command, stdin}` | `PROGRAM -c COMMAND` | `stdin` |
| `edit {path, old, new, all}` | `PROGRAM PATH OLD NEW [--all]` | empty |
| `job {id, …}` | nothing: the kernel acts on the job ([below](#job)) | |

The program runs by name, so its `argv[0]` is its own: busybox, which picks
what to run from `argv[0]`, runs its `sh`. A `PATH` that starts with `-` is
passed as `./PATH`. Because every call runs a program, a wrapper named as a
tool's `program`, or a shim earlier on `PATH` (§2), sees, refuses or rewrites
every call of a tool. `edit` calls are like `shell` calls in every other way: they run in parallel, count against `--max-processes`, become jobs
when a message arrives first, and are killed by `abort`. Their result is the
program's output (stdout and stderr share a pipe, as for `shell`) when it
exits 0; otherwise an error: that output without its final newline, else
`PROGRAM: exit N`.

**bash or sh.** `make` installs `tools.json` as is when `bash` is on `PATH`;
otherwise with `sh` as the shell's `program` and in its description ("Run a
command with sh and …"), so the model is told the syntax that runs. The kernel and
the system-prompt program know nothing of either.

| Call | Result |
|---|---|
| a name not in `tools.json` | error: `Unknown tool: X. The tools are shell, job and edit.` (the names in the file) |
| `input` not a JSON object | error: `input is not a JSON object: ` and the text, clipped to 200 bytes (`…` when clipped) |
| `shell` without `command`, `job` without `id`, `edit` without `path` | error: `command is required`, `id is required`, `path is required`; nothing runs |

Every result is a `tool_result` (or, for a job, a later `job_result`) with a
block array as `content`; an empty text becomes `(empty)`. How calls run in
parallel, wake the turn and become jobs: [calls.h](../akern/calls.h) and **Jobs** below.

### shell

| Field | Meaning |
|---|---|
| `command` | run as `bash -c COMMAND` (or the shell's `program`), a fresh shell in the kernel's working directory each time: `cd`, variables and activated environments do not carry over to the next call. For a single simple command, bash runs it in its own place, so the pid is the program's |
| `stdin` | text written to the program's stdin, which is then closed (unless `eof: false`). No escaping, no argument size limit: `cat > path` with the file's text here is how the model writes a file |
| `eof` | `false`: stdin stays open after the `stdin` text; the model sends more with `job {id, stdin}` and ends it with `job {id, eof: true}`. Default `true` |
| `wait` | seconds the turn waits for the command; absent, until it finishes. When it runs out, or a message arrives first, the call is answered pending and runs on as a job. `0`: a job from the start (a server, a watcher, a sub-agent it steers). `false` and `true` are taken as `0` and absent |
| `stdout_name` | the `name` of a document block (below) |
| `output_limit` | bytes of output kept in the result; default 100000; ignored for an image or PDF |

| Program side | |
|---|---|
| stdin | a pipe: the `stdin` text, then EOF; EOF at once without it; with `eof: false`, EOF after a `job` call with `eof` |
| stdout, stderr | one pipe for both, as `2>&1`: the output in the order it was written (as the program flushes it: most programs buffer stdout to a pipe but not stderr), collected in full |
| exit status | reported as is; anything but `0` sets `is_error` |
| process group | its own, so a signal to the group reaches everything it started |
| environment, cwd | the kernel's, plus [akern.md §1](akern.md#set-for-children) |
| not found | the shell's message on stderr, exit 127 (`bash: no-such: command not found`); a missing shell, `bash: No such file or directory`, exit 127 |

Nothing else passes between the program and the kernel; a program cannot
change how its call is handled. A wrapper named as the shell's `program` gets
`-c COMMAND` and can refuse it (exit non-zero, a reason on stderr) or run
something else, e.g. `exec bash "$@"`; its output and exit status are the
result.

**Result.** One text block, plain text, so code and logs reach the model
unescaped. Its first line is always a header in brackets; everything after
that line is the output, as is. The header is there even for `[exit 0]`, so
that a line the program prints that looks like one is only ever output: the
program cannot write before it. The header's parts, in this order, joined by
`; `:

| Part | When |
|---|---|
| `exit N`, `signal N` | always: how it ended; `killed, signal N` (or `killed, exit N` when it caught the signal) when the kernel killed it: `abort` with `jobs`, or shutdown; `killed: output over 67108864 bytes, signal N` when its kept output passed 64 MiB |
| `cut at N of M bytes, all of it in FILE` | the output was longer than `output_limit`: the result keeps its first N bytes (a character cut in two shows as U+FFFD) and all of it is in FILE |
| `M bytes of binary output not shown` | the output is not text: a NUL byte, or more than one byte in eight outside UTF-8, in its first 8 KiB. `M bytes of image/png not shown` (or another type, below) when the program failed. `, all of it in FILE` follows when it spilled |

When FILE could not be created or written, `, saving all of it to FILE failed:
REASON` replaces `, all of it in FILE` (`saving output to FILE failed: REASON`
when nothing was cut), and FILE keeps only what was written before. With no
output the result is the header alone.

```text
[exit 2]
src/main.c:12: warning: unused variable 'n'
make: *** [all] Error 1
```

A non-zero `exit N` also sets `is_error`; the header says it anyway, since not
every provider carries `is_error`.

Output is UTF-8 text; invalid bytes become U+FFFD.

**Images and PDFs.** When the output starts with the signature of a format
every provider takes, the kernel keeps all of it and, if the program succeeds,
returns it as a media block. Since stderr shares the pipe, a message the
program writes there lands inside the image; the description tells the model
to send stderr elsewhere (`magick in.tiff png:- 2>err.txt`).

| First bytes | Block |
|---|---|
| `89 50 4E 47 0D 0A 1A 0A` | image, `image/png` |
| `FF D8 FF` | image, `image/jpeg` |
| `GIF87a`, `GIF89a` | image, `image/gif` |
| `RIFF`, 4 bytes, `WEBP` | image, `image/webp` |
| `%PDF-` | document, `application/pdf`, named `stdout_name` if given |

So `cat shot.png` shows the model the image. A failed run returns the header
alone, e.g. `[exit 1; 48213 bytes of image/png not shown]`: the bytes may be
cut short, and a provider refuses a broken image. A block the API rejects is replaced with a note in the session
([akern.md](akern.md#session-file)).

**Jobs.** A call answered with a `tool_pending`, or started with
`wait: 0`, is a job named by its call id. From the moment it is answered
pending (or starts) its output is
written whole to `<session>.d/calls/<call>.out` (output that already spilled
keeps its file), and the `tool_pending` names it with the pid. The model watches with `tail`, acts on it with [job](#job), and waits by
ending its turn (the `job_result` starts a new one) or with `job`'s `wait`. Its output is also forwarded live as `tool_progress` events, except an image's or PDF's.

**Limit.** A call that would run more than `--max-processes` programs at
once fails at once, with status `not run` and the text
`Not run: already at the --max-processes limit (N).`

### job

`job {id, stdin?, eof?, signal?, wait?}` acts on the job named `id`, in the
kernel: it starts no program and counts against no limit. In order:

| Field | Does |
|---|---|
| `stdin` | appends the text to the job's stdin, after anything still queued there; the job must have been started with `eof: false` |
| `eof` | `true`: closes the job's stdin after that, so the program sees EOF |
| `signal` | sends a signal to the job's process group: `HUP`, `INT`, `QUIT`, `KILL`, `USR1`, `USR2`, `TERM`, `STOP`, `CONT` (with or without `SIG`), or a number. The job's result still arrives |
| `wait` | seconds to wait for the job to end. It stays a job either way: when it ends, its `job_result` arrives in the same message as this call's reply |

| Reply | When |
|---|---|
| `ID: wrote N bytes to its stdin, closed its stdin, sent it SIGTERM.` | the steps taken, without `wait` |
| `ID is running.` | no step and no `wait` |
| `ID: …; ended: exit 0.` (or `ended: signal 15`), `ID ended: …` | the job ended within `wait` |
| `ID: …; still running after N s.`, `ID still running after N s.` | `wait` ran out |
| `"ID" is not a running job.` | no job with that id has a process; nothing is done |
| `The stdin of ID is closed.` | `stdin` or `eof` given, and its stdin is closed; nothing is done |
| `Unknown signal: X` | nothing is done |

A `job` call with `wait` is a call like any other: a message that arrives
first answers it pending, and its own `job_result` follows. The job's stdin
pipe never leaves the kernel. It is anonymous: only the kernel holds its write
end, so there is nothing to clean up. It is the handle POSIX gives the parent,
which is why the kernel writes for the model rather than exposing the job's
stdin as a path (`/proc/PID/fd/0` would be Linux-only and subject to pid
reuse).

### edit

`edit {path, old, new, all?}`, carried out by `akern-edit PATH OLD NEW [--all]`
(§2); each argument reaches it byte for byte. Replaces `old` with `new`; `old`
must match exactly once unless `all` is true. The file is replaced atomically, keeping its mode.

| Reply | When |
|---|---|
| `Edited PATH: replaced N occurrence(s). First change at line L:` and the changed lines with 3 lines of context, numbered | success |
| `akern-edit: PATH: OLD text not found`, `… OLD text matches N times`, `… OLD must not be empty`, or the system's reason | failure; the file is unchanged |

## 2. Programs on PATH

Any program on `PATH` is a tool: people, scripts and the model run the same
programs, the model through `shell`.

| Convention | Rule |
|---|---|
| lookup | by name; there is no dispatcher, `PATH` already is one |
| `akern-<cmd> --help` | one line: usage, then what it does. No listing command: skills are the model's index, people tab-complete `akern-` |
| no wrappers | when a wrapper seems needed, the API it would wrap is missing something, and that is fixed instead. Listing, stopping and waiting for jobs need no command (§1, Jobs) |
| no write command | files are written with `cat > path` in the shell, the content on its stdin, so it never passes through quoting and its escape traps (heredocs expanding `$` and backticks, `echo` reading backslashes, `printf` reading `%`). The model runs `mkdir -p` first for a new directory |
| session commands | take the session path as the first argument; write to the file only when it is not owned ([akern.md](akern.md#writing)); an owned session is changed through its kernel's stdin |
| slash commands | an interactive front end maps `/cmd args` to `akern-cmd SESSION args` and shows its stdout |
| shims | a program earlier on `PATH` wraps another: check, rewrite, log, redact. Shims must be reentrant, since parallel calls run them at once. A shim of `akern-edit` sees every call of its tool; to run the real one, it drops its own directory from `PATH` and `exec`s the name. For `shell`, name a wrapper as its `program` in your `tools.json` instead: a shim of `bash` would catch every bash on the machine |

```sh
#!/bin/sh
# ~/shims/akern-edit, with ~/shims first on PATH
case "$1" in */.ssh/*) echo "akern-edit: $1: refused" >&2; exit 1;; esac
PATH="${PATH#*:}" exec akern-edit "$@"
```

### akern-edit

`tools/akern-edit.c`, self-contained C: the program behind `edit` (§1).

```
akern-edit PATH OLD NEW [--all]
```

Replaces OLD with NEW byte for byte; OLD must match exactly once unless
`--all`. The file is replaced atomically, keeping its mode.

| Exit | Output |
|---|---|
| 0 | `--help` alone or no arguments: the usage line |
| 0 | `Edited PATH: replaced N occurrence(s). First change at line L:` and the changed lines with 3 lines of context, numbered |
| 1 | on stderr, `akern-edit: PATH: ` and one of `OLD must not be empty`, `OLD text not found`, `OLD text matches N times`, or the system's reason; wrong arguments: `akern-edit: usage: …` |

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
description: How to use this agent harness
---

# Using akern
…which command does what, and when…
```

| Item | Rule |
|---|---|
| location | yours in `$AKERN_HOME/skills/<name>/SKILL.md`, then the shipped ones in `$AKERN_HOME/system/skills/<name>/SKILL.md` (`skills/<name>/SKILL.md` in the repository). A directory in `skills/` hides the shipped skill of the same name, with or without a `SKILL.md` |
| header | `name:` (default: the directory name) and `description:`, one line each, in the front matter between the first two `---` lines |
| list | `Skills: read a skill's file before relying on it.`, then one line per skill: `- NAME: DESCRIPTION (absolute path of SKILL.md)`; yours, then the shipped ones, each in byte order of directory name. No skills: no list |
| use | the [system-prompt program](#the-system-prompt-program) puts the list in the system prompt; the model reads a skill's file (`cat` in the shell) before relying on it, so only the list costs context on every request |

| Shipped | Covers |
|---|---|
| `skills/akern` | sub-agents: the `akern` command's flags, exit codes and stdin, steering, watching and continuing one, with examples ([akern.md](akern.md#sub-agents)) |

What the model reads, and when: each tool's description says what its fields
do; the [base prompt](#the-system-prompt-program), on every request, says how to work
with the tools; a skill, read only when needed, covers one occasional task in
depth and repeats a field only inside a recipe.

### The system-prompt program

`tools/system-prompt`, `sh`, shipped as `$AKERN_HOME/system/system-prompt`: the kernel runs
it once at startup, and its output is the system prompt
([akern.md §2](akern.md#system-prompt)). It reads markdown and runs nothing
from the project. Its parts, in order, separated by a blank line, each ending
in a newline:

| Part | Content |
|---|---|
| base prompt | `$AKERN_HOME/base-prompt.md`, else the shipped `$AKERN_HOME/system/base-prompt.md` (`tools/base-prompt.md`), verbatim, when not empty: how to work in the harness (parallel calls, refusals, asking before anything hard to undo, editing, jobs, images). An empty file of yours leaves it out |
| environment | `Your working directory is PATH.`; in a repository (as below), `It is in the git repository ROOT.`; `Today's date is YYYY-MM-DD.`; `The system is` and `uname -sr`. Always |
| your text | `$AKERN_HOME/system.md`, verbatim, when not empty |
| skills | the [list](#3-skills) |
| context files | for each directory from the repository root (the nearest ancestor of the working directory, itself included, that has a `.git`) down to the working directory, outermost first: its `AGENTS.md`, else its `CLAUDE.md`, when not empty, under a heading `# ABSOLUTE PATH`. Outside a repository, the working directory alone |

Its home is the directory above its own, since it is shipped as
`HOME/system/system-prompt`, so `./build/system/system-prompt` prints the build's prompt
whatever `AKERN_HOME` says; a copy anywhere else uses `$AKERN_HOME`, else
`~/.akern`. The output depends only on these files, so kernels started on the
same files, in the same directory on the same day, send the same prompt and
keep the provider's cache warm. A kernel keeps the prompt it started with: a
session that runs past midnight still says the day it began. Your own
`$AKERN_HOME/system-prompt` replaces this program; it can run it first
(`~/.akern/system/system-prompt`) and add to its output.

### Install

| `make` | `make install` (`PREFIX=/usr/local`, `AKERN_HOME=~/.akern`) |
|---|---|
| `build/bin/`: `akern`, `akern-edit`, `akern-cli`, `akern-model` | the same programs into `$PREFIX/bin` |
| `build/system/`: `providers/`, `skills/`, `render.jq`, `system-prompt`, `base-prompt.md`, `tools.json`, `README.md` (from `home/README.md`: the layout and how to override) | copied to `$AKERN_HOME/system/`, which is removed first |

`build/` has the layout of an installed `$AKERN_HOME`, so `AKERN_HOME=build`
(what `./akern-dev.sh` sets) runs the build as if installed.
`AKERN_HOME` is the variable `akern` reads, so one exported in the shell
sets both where `make install` writes and where the programs look. An
`AKERN_HOME` that is the build directory fails `make install` and
`make uninstall`.
`$AKERN_HOME/system` is the base layer and belongs to `make install`, which
replaces it whole: a file dropped from a release does not linger. The user
layer is `$AKERN_HOME` itself, looked up first: `providers/<name>`, `render.jq`,
`system-prompt`, `tools.json`, and what the shipped `system-prompt` reads, `base-prompt.md`, `system.md` and `skills/`. A
shadowing file is reported by the install. Install writes nothing there, and
removes only a copy that changes nothing: a provider, `render.jq`, `system-prompt`,
`base-prompt.md` or `tools.json` byte-identical to the new one in `system/` is deleted and reported; one that
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
