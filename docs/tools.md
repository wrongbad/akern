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
{"name":"shell","program":"akern-shell","input_schema":{"type":"object","properties":{
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
| `shell {command, stdin, output_limit, stdout_name}` | `PROGRAM -c COMMAND [--output-limit N] [--stdout-name NAME]` | `stdin` |
| `edit {path, old, new, all}` | `PROGRAM PATH OLD NEW [--all]` | empty |
| `job {id, …}` | nothing: the kernel acts on the job ([below](#job)) | |

A `PATH` that starts with `-` is passed as `./PATH`. Because every call runs a
program, a wrapper named as a tool's `program`, or a shim earlier on `PATH`
(§2), sees, refuses or rewrites every call of a tool. `edit` calls are like
`shell` calls in every other way: they run in parallel, count against
`--max-processes`, become jobs when a message arrives first, and are killed by
`abort`.

**A program's side.** Every program the kernel runs for a call gets the same:

| | |
|---|---|
| stdin | a pipe: the `stdin` text, then EOF; with `eof: false`, EOF after a `job` call with `eof` |
| stdout, stderr | one pipe for both, as `2>&1`, in the order written; streamed as `tool_progress` |
| fd 3 | a pipe for the result: a JSON array of content blocks. A program that writes nothing there is answered with what it printed (at most 100000 bytes), and, when it failed, as an error: that without its final newline, else `PROGRAM: exit N` |
| `AKERN_OUT` | `<session>.d/calls/<call>.out` ([akern.md §5](akern.md#layout)), the file for its whole output, written by the program if at all |
| exit status | anything but `0`, or a kill, sets `is_error` |
| process group | its own, so a signal to the group reaches everything it started |
| environment, cwd | the kernel's, plus [akern.md §1](akern.md#set-for-children) |

Nothing else passes between a program and the kernel. The shell's program is
[akern-shell](#akern-shell), which formats the result; a wrapper named as the
shell's `program` gets the same arguments and, unless it writes fd 3 (`exec
akern-shell "$@"` does), returns what it printed as is.

**bash or sh.** `akern-shell` runs `bash`, else `sh`. `make` installs
`tools.json` as is when `bash` is on `PATH`; otherwise with `sh` in the
shell's description ("Run a command with sh and …"), so the model is told the
syntax that runs. The kernel and the system-prompt program know nothing of
either.

| Call | Result |
|---|---|
| a name not in `tools.json` | error: `Unknown tool: X. The tools are shell, job and edit.` (the names in the file) |
| `input` not a JSON object | error: `input is not a JSON object: ` and the text, clipped to 200 bytes (`…` when clipped) |
| `shell` without `command`, `job` without `id`, `edit` without `path` | error: `command is required`, `id is required`, `path is required`; nothing runs |

Every result is a `tool_result` (for a job, a late one, which also names how it ended) with a
block array as `content`; an empty text becomes `(empty)`. How calls run in
parallel, wake the turn and become jobs: [tools.c](../akern/tools.c) and **Jobs** below.

### shell

| Field | Meaning |
|---|---|
| `command` | run by [akern-shell](#akern-shell) as `bash -c COMMAND`, a fresh shell in the kernel's working directory each time: `cd`, variables and activated environments do not carry over to the next call. The call's pid is akern-shell's, and its process group the command's |
| `stdin` | text written to the program's stdin, which is then closed (unless `eof: false`). No escaping, no argument size limit: `cat > path` with the file's text here is how the model writes a file |
| `eof` | `false`: stdin stays open after the `stdin` text; the model sends more with `job {id, stdin}` and ends it with `job {id, eof: true}`. Default `true` |
| `wait` | seconds the turn waits for the command; absent, until it finishes. When it runs out, or a message arrives first, the call is answered pending and runs on as a job. `0`: a job from the start (a server, a watcher, a sub-agent it steers). `false` and `true` are taken as `0` and absent |
| `stdout_name` | the `name` of a document block (below) |
| `output_limit` | bytes of output kept in the result; default 100000; ignored for an image or PDF |

What akern-shell does with the command (§1 has the rest of a program's side):

| | |
|---|---|
| shell | `bash -c COMMAND`, else `sh -c COMMAND`; its stdout and stderr on one pipe, in the order written (as the program flushes it: most programs buffer stdout to a pipe but not stderr) |
| output | passed on to the kernel as it comes, and written whole to `AKERN_OUT` |
| exit status | akern-shell exits as the command did; anything but `0` sets `is_error` |
| signals | a signal to the call's process group reaches the command; akern-shell outlives it to write the result |
| not found | the shell's message on stderr, exit 127 (`bash: no-such: command not found`) |

**Result.** akern-shell's, on fd 3: one text block, plain text, so code and logs reach the model
unescaped. Its first line is always a header in brackets; everything after
that line is the output, as is. The header is there even for `[exit 0]`, so
that a line the program prints that looks like one is only ever output: the
program cannot write before it. The header's parts, in this order, joined by
`; `:

| Part | When |
|---|---|
| `exit N`, `signal N` | always: how the command ended. When the kernel killed it (`abort` with `jobs`, or shutdown), a job's result says `killed` in its `status` |
| `cut at N of M bytes, all of it in FILE` | the output was longer than `output_limit`: the result keeps its first N bytes (a character cut in two shows as U+FFFD) and all of it is in FILE |
| `M bytes of binary output not shown, all of it in FILE` | the output is not text: a NUL byte, or more than one byte in eight outside UTF-8, in its first 8 KiB. `M bytes of image/png not shown` (or another type, below) when the program failed |

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
every provider takes, akern-shell keeps all of it (up to 32 MiB) and, if the program succeeds,
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

**Jobs.** A call still running when its turn is answered (its `wait` ran
out, or a message arrived first), or started with `wait: 0`, is a job named by
its call id. Its output is in `<session>.d/calls/<call>.out` as it comes,
as is every shell call's. The provider tells the model so in a placeholder answer that
names the job and that file ([providers.md §5](providers.md#5-messages)). The model watches with `tail`, acts on it with [job](#job), and waits by
ending its turn (its late result starts a new one) or with `job`'s `wait`. Its output is also forwarded live as `tool_progress` events, except an image's or PDF's.

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
| `wait` | seconds to wait for the job to end. It stays a job either way: when it ends, its late result arrives in the same message as this call's reply |

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
first answers it pending, and its own late result follows. The job's stdin
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

### akern-shell

`tools/akern-shell.c`: the program behind `shell` (§1), which turns a command's
output into its result.

```
akern-shell -c COMMAND [--output-limit N] [--stdout-name NAME]
```

Runs `bash -c COMMAND` (`sh` where there is no bash), passes its output on to
stdout as it comes and writes all of it to `$AKERN_OUT`, then writes the
result described under [shell](#shell) to fd 3, or to stdout when fd 3 is not
open, so a person can run it by hand. Exits as the command did.

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

### akern-agent

`tools/akern-agent.c`: a sub-agent that speaks [ACP](acp-v2.md) protocol 2,
run the way `akern -p` runs an akern sub-agent ([akern.md](akern.md#sub-agents)),
so the model delegates to any ACP agent with the same habits: a task as an
argument, the answer on stdout, steering lines on stdin, the same `-s` to
continue. It is an ACP client, the other side of `akern-acp` (§5).

```
akern-agent [-a AGENT_COMMAND] [-s SESSION.jsonl] [-C DIR] [-m MODEL] [-o ID=VALUE]...
            [--allow] PROMPT...
```

| Flag | Meaning |
|---|---|
| `-a`, `--agent` | the command that starts the agent, run with `/bin/sh -c` in the session's cwd. Required for a new session; a continued one uses the command in its header |
| `-s`, `--session` | the session file: created if missing, continued (`session/resume`, no replay) if it holds a header. Locked with `fcntl` while in use; held: exit 2. Default, `$TMPDIR/akern-agent-XXXXXX` |
| `-C`, `--cwd` | the session's `cwd`; default, the current directory, or a continued session's own |
| `-m`, `--model` | `-o model=VALUE` |
| `-o`, `--option` | `session/set_config_option` before the prompt: the option whose `configId`, else whose `category`, is `ID`; a select's choice by value, else by value or name ignoring case; a boolean as `true`/`false`. Unknown: exit 2, listing the options or choices |
| `--allow` | permission requests are answered with `allow_once` (else `allow_always`) |

| What | How |
|---|---|
| handshake | `initialize` with `protocolVersion` 2 and no client capabilities; an agent that answers another version: exit 2 |
| prompt | the arguments, joined with spaces, as one text block |
| stdin | each line while it runs: plain text is a `session/prompt`; `{"type":"send","content":…}` too, after a `session/cancel` when `"when":"now"`; `{"type":"abort"}` is `session/cancel`. EOF changes nothing |
| stdout | at each `state_update` `idle` of its session: the text of the last agent message with text since the previous one, and `\n` unless it ends in one |
| done | every prompt answered (inserted), the session `idle` in a state reported after the last answer or after work begun since the last prompt, and its `background` list empty or not reported, for 0.5 s. Then it closes the agent's stdin, and after 5 s sends `SIGTERM`, after 2 s more `SIGKILL` |
| permission | without `--allow`: answered `reject_once` (else `cancelled`), then `session/cancel`, so the work stops now; stderr names what was refused and how to continue |
| other agent requests | `-32601` (it offers no elicitation) |
| child sessions | their updates and requests go to the file and through the same permission policy; they print nothing |
| signals | `SIGINT`, `SIGTERM`, `SIGHUP`: `session/cancel`, the agent stopped as above, exit 130 |

| Exit | When |
|---|---|
| 0 | done; a stop reason other than `end_turn` or `cancelled` is named on stderr |
| 1 | the last idle was `stopReason: error` (its message on stderr); the agent exited before it was done (with the end of its stderr); or it refused the first prompt |
| 2 | usage; the session file cannot be opened, is not one, or is in use; setup failed (the agent would not start, `initialize`, `session/new`, `session/resume` or an option failed, with the agent's message) |
| 3 | done, and a permission request was refused |
| 130 | a signal |

The session file is JSON lines: a header `{"type":"acp_session","sessionId",
"agent","cwd"}`, then every line of the connection as `{"in": MESSAGE}` from
the agent or `{"out": MESSAGE}` to it, byte for byte (a line from the agent
that is not JSON: `{"in_text": "…"}`). The agent's stderr is appended to
`SESSION.jsonl.stderr`. The agent runs in `akern-agent`'s process group, so a
parent's signal to the job reaches both.

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
| `skills/acp` | sub-agents that speak ACP: the `akern-agent` command, permissions, steering, continuing one, with examples ([§2](#akern-agent)) |

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
| `turn_start`, `run_end` | `↳ ID keeps running` for each call started and not answered, once, dim |
| `block` with a `tool_result` | `✓ CALL`, plus ` (error)`, dim |
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
| `⏵ N jobs running · waiting for input` | otherwise, while some call has a `tool_start` and no `block` yet |
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

## 5. The akern-acp front end

`tools/akern-acp.c`, for editors and hosts that speak the
[Agent Client Protocol](https://agentclientprotocol.com) (Zed, Omnigent's
`acp:` agents): JSON-RPC 2.0 lines on stdin and stdout. It runs one `akern`
per ACP session and translates; it adds no behavior of its own.

```
akern-acp [--until run|settled] [--background hold|send] [--sessions DIR] [-- akern args...]
```

| ACP | akern |
|---|---|
| `initialize` | protocol 1; `loadSession`, images and embedded resources in prompts; no auth methods |
| `session/new` | `akern -s DIR/ID.jsonl [args]` in the session's `cwd`, answered once `hello` arrives. `models`: the current model, then the others its provider lists with `--models` (asked once per provider, within 10 s), as `PROVIDER/ID`. `configOptions`: `effort`, category `thought_level`, a choice of `default`, `low`, `medium`, `high`. A client-chosen `sessionId` of letters, digits, `-` and `_` is kept. `mcpServers` are ignored, with a line on stderr: akern has no MCP client |
| `session/load` | replays the context ([akern.md §5](akern.md#session-file)) as `user_message_chunk`, `agent_message_chunk`, `agent_thought_chunk`, `tool_call` and `tool_call_update`, then starts `akern` on it with `-m` and `-P effort=` from the file's last `hello`, so it keeps the model and effort it had; answered like `session/new` |
| `session/prompt` | `send`: text as text, images as images, a `resource` as `<resource uri="…">` text, a `resource_link` as `@URI` |
| `session/cancel` | `abort` (jobs keep running), sent at once: the run ends `aborted`, or akern hands the prompt back (`returned`); either way the prompt ends `cancelled`. A prompt already delivered whose run has ended, with only jobs left (`--until settled`), ends `cancelled` at once |
| `session/set_model`, `session/set_config_option` (`model`) | the session's akern starts again with `-m MODEL` on the same file: at once when it has settled, else once it does (prompts sent meanwhile still use the old model) |
| `session/set_config_option` (`effort`) | likewise, with `-P effort=LEVEL`, or none for `default`; then a `config_option_update` |
| a prompt that is only `/model`, `/model ID`, `/effort` or `/effort LEVEL` | answered by `akern-acp` and never sent to the model, for clients with no model or effort control of their own (Omnigent): `/model` lists the provider's models, `/model ID` switches (a bare id stays with the current provider), `/effort` sets the param as the option does, any value the provider takes |

| akern event | ACP `session/update` |
|---|---|
| `text_delta`; an assistant `message` whose text was not streamed | `agent_message_chunk` |
| `thinking_delta` | `agent_thought_chunk` |
| `tool_start` | `tool_call`, `in_progress`: `title` the command (or `edit PATH`, `job ID`), `kind` `execute`, `edit` or `other`, `rawInput` the input; an edit adds its absolute path as `locations` and a `diff` |
| `tool_progress` | `tool_call_update`, `in_progress`: the last 16 KiB of output, fenced, at most every 0.25 s |
| `block` with a `tool_result` | `tool_call_update`, `completed` or `failed` (`is_error`): its text fenced for `shell`, its images, an edit's diff first; `rawOutput` the block |

A prompt is answered when akern has dealt with it, with `stopReason`
`end_turn` (`cancelled` after a cancel or an abort) and `usage`
(`inputTokens`, `outputTokens`, `cachedReadTokens`, `cachedWriteTokens`,
`totalTokens`) summed over its turns. A run that ends in `error` answers it with
a JSON-RPC error, `data` the run's error.

| Flag | Default | Meaning |
|---|---|---|
| `--until` | `run` | `run`: at the `run_end` of the run that took the prompt, so a client waits only for the model. `settled`: at `settled`, so a prompt also waits for its jobs and the runs their results start |
| `--background` | `hold` | what akern does while no prompt is open (a job ends and wakes the model): `hold` keeps those updates and sends them first when the next prompt starts, since many clients drop updates outside a prompt; `send` sends them at once |
| `--sessions` | `$AKERN_ACP_SESSIONS`, else `$XDG_STATE_HOME/akern/acp`, else `~/.local/state/akern/acp` | where session files go; a restarted `akern-acp` loads them by id |
| after `--` | | passed to every `akern`, after `-s`: `-m`, `-P`, `-S`, limits |

### Protocol 2

A client that asks `initialize` for `protocolVersion` 2 or more gets protocol 2,
the [v2 draft](https://agentclientprotocol.com/rfds/v2/overview); 1 gets the
above. The draft may still change. Protocol 2 has no prompt to hold work for, so
akern's concurrency shows as it is: `--until` and `--background` do not apply.

| ACP 2 | akern |
|---|---|
| `initialize` | protocol 2, `info`, `capabilities.session` with images and embedded resources in prompts; no auth methods |
| `session/new` | as protocol 1; `configOptions` are `model` (category `model`: the current model and the ones its provider lists) and `effort`; `availableCommands` are `model` and `effort` |
| `session/resume` | starts `akern` on the file, then a `state_update` (`idle`) and a `session_info_update` with the title. `replayFrom: {"type":"start"}` first replays the context, in the kernel's order (a reply where its `turn_start` is), as whole `user_message`, `agent_message`, `agent_thought` and `tool_call_update`s, then `replay_complete`. Each message keeps the id it had live: an assistant message its entry id; a user message one `user_message` per prompt or inject item in its `meta.items`, by the item's id, with that item's blocks (a message without `meta.items`: one, by its entry id). A call the file leaves open is `failed`: the akern that ran it is gone |
| `session/list` | the files in `--sessions` whose header `cwd` is the one asked for: title from the last `name` entry, else the first prompt's first line; `updatedAt` the file's mtime |
| `session/close` | `abort` with `jobs`, then stdin closed; answered once `akern` has exited |
| `session/prompt` | `send`, at any time. Answered `{"messageId"}`, the item id its `ok` gave, then a `user_message`, once akern delivers the item (a `message` whose `meta.items` name it): ACP's insertion, so a prompt queued behind running calls or with `when` `idle` is answered later. Handed back by an abort before that (`returned`, or `run_end.items`): answered `-32800`, never inserted. While a run works it steers it: `when` `turn`, delivered at its next turn. `_meta` `io.github.wrongbad.akern/when` picks another [delivery mode](akern.md#delivery-modes) (`now` preempts the streaming turn) |
| `session/cancel` | `abort` at once, idle or not: akern ends the run and hands back what it has not delivered, which answers those prompts `-32800`; jobs keep running. On a child session: `SIGTERM` to the call that runs it |
| `tool_call/cancel`, `terminal/cancel` | `SIGTERM` to the call's process group; its result closes it `cancelled` |
| `session/set_config_option` | `model` or `effort` (`type` `id`), as in protocol 1 |
| `/model`, `/effort` prompts | as in protocol 1, run from idle to idle, the prompt a live-only `user_message` under an id `command-` and 16 hex, which no akern id can be |

| akern event | ACP 2 `session/update` |
|---|---|
| `run_start` | `state_update` `running` |
| `run_end` | `state_update` `idle`, `stopReason` `end_turn`, `cancelled` (aborted, or after a cancel) or `error` with `error.data` the run's error; `_meta` `io.github.wrongbad.akern/usage` the run's usage summed |
| either | `background`: each call still open from an earlier run (a job), as a `tool_call`, and a `subagent` when it runs one |
| `text_delta`, `thinking_delta` | `agent_message_chunk`, `agent_thought_chunk`, by the `messageId` `turn_start.id`: the id the turn's assistant entry gets |
| an assistant `message` | `agent_thought` and `agent_message` with its whole content, by the same id |
| `tool_start` | `tool_call_update` creating the call, `in_progress`, as in protocol 1. A `shell` call that started is also a terminal: `terminal_update` with its `command` and `cwd`, and `{"type":"terminal"}` content. A call with a process has `capabilities.cancel`. An edit's content is a `diff` with its file in `changes` |
| `tool_progress` | `terminal_output_chunk`, base64, at most every 0.25 s or past 1 MiB |
| `block` with a `tool_result` | `tool_call_update` `completed`, `failed`, or `cancelled` (killed by an abort or a `tool_call/cancel`). A terminal gets `exitStatus` from its result's header. An edit's `diff` gains a `git_patch`: where its new text now is, widened to whole lines, when it is there once |

A job runs on after its run: its updates come when they happen, and when it
ends its result starts a run of its own, `running` then `idle` with no prompt.

A `shell` call whose command runs `akern … -s FILE` (a sub-agent, [akern.md
§5](akern.md#sub-agents)) is a child session, `SESSION/PATH` with `PATH` the file
relative to the session's directory without `.jsonl`. It is announced with
`subagent_update` (`running`) before the call that names it in a
`{"type":"session"}` content item, and the file is read every 0.2 s while the
call runs: its user text is a `session_message` from the parent, its assistant
messages `agent_message` and `agent_thought`, its calls `tool_call_update`s,
its own sub-agents children of the child. When the call ends, the file is read
once more, and the child goes `idle`: `end_turn`, `error`, or `cancelled`.
The same file run again is the same child.

The model is `AKERN_MODEL`, else the first line of `$AKERN_HOME/model`, as for
`akern-cli`. akern's stderr goes to `akern-acp`'s, each line prefixed
`akern[ID]:`; when a session cannot start, the prompt or `session/new` fails
with it.

Zed, in `settings.json`:

```json
{"agent_servers": {"akern": {"command": "akern-acp", "args": []}}}
```

Omnigent, as a user-configured ACP agent (`acp:akern`).
