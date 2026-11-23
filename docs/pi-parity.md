# Pi parity map

The claim in [design.md](design.md) is that every pi feature can be
supplied without changing the kernel. This is the evidence: each feature, the
seam that provides it, and roughly how.

Feature list taken from pi's documentation as of November 2026.

**Where** column: **Kernel** is `akern` itself; **User (bundled)** is a program or skill shipped in this repo; **User** is anything else, built on the documented contracts, including what [roadmap.md](roadmap.md) plans.

## Core loop and tools

| Pi feature | Where | Mechanism |
|---|---|---|
| Agent loop with streaming | Kernel | `akern` |
| `bash` | Kernel | the built-in `shell` tool: `akern-shell`, which runs `bash -c COMMAND`, or `sh` where there is no bash |
| `read` | — | `cat`, `head`, `tail`, `sed -n` through `shell`; an image or PDF on stdout comes back as an image or document block |
| `write` | — | `cat > path` (or `>>`) through `shell`, the text on stdin, so nothing needs quoting |
| `edit` | Kernel | the built-in [`edit`](tools.md#edit) tool, run as `akern-edit`: exact-match replace, once or `all`; the text passes as arguments, byte for byte, so it needs no quoting |
| `grep`, `find`, `ls` | — | the system utilities, through `shell` |
| Tool allowlist / exclude (`--tools`, `-xt`, `-nt`) | User | a restricted `PATH`, or a shim on `PATH` that refuses: of one program, or of `akern-edit`, or a wrapper named as a tool's `program`, for a whole tool; or a `tools.json` without it |
| Custom tools | User | a command on `PATH` plus `skills/<name>/SKILL.md`, which the model runs through `shell`; `tools.json` cannot add a tool: its `program` can only replace what runs behind `shell` or `edit` ([tools.md](tools.md#1-built-in-tools)) |
| Tool progress updates | Kernel | stderr chunks → `tool_progress` events |
| Parallel tool calls | Kernel | every `tool_use` in a message starts at once, up to `--max-processes` |
| Abort | Kernel | `abort` command, `SIGINT` |
| Steering (delivered after current tools finish) | Kernel | `send` with `when: "turn"`, which is also what a plain line typed while the agent works becomes |
| Follow-up (delivered when the agent finishes) | Kernel | `send` with `when: "idle"` |
| Return queued messages to the editor | Kernel | `run_end.returned` after `abort` |

## Beyond pi: concurrency the kernel provides

Pi leaves these to extensions or does not do them. Here they are kernel
mechanisms, because they are what tests whether the architecture holds.

| Feature | Where | Mechanism |
|---|---|---|
| Immediate steering (interrupt a turn mid-stream or mid-tool) | Kernel | `send` with `when: "now"` interrupts a streaming response; any message wakes a turn waiting on tools, whose unfinished calls are answered pending and keep running as jobs |
| Background tool calls | Kernel | `"wait": false` on the call, or a message arriving while calls run ([tools.md: exec](tools.md#exec)) |
| Job results that wake the agent | Kernel | every finished job's result is queued `turn` and wakes it; only `abort` defers results already queued |
| Model-visible job control | Kernel | a pending call's answer names its pid and output files; `tail`, `kill -TERM -- -PID` through `shell`; waiting is ending the turn. People also have `{"type":"status"}` on stdin |
| Talking to a running program (REPL, sub-agent) | Kernel | `shell` with `eof: false` keeps the program's stdin open; the model sends it more with `job {"id":"<call id>","stdin":…}`, and `eof` ends it. Steering from another terminal is not supported: whoever started the kernel drives it (use tmux) |
| Model switch without losing background work | — | no: another model is another start on the same session (`-s S -m other`), once its jobs are done |

## Modes

| Pi feature | Where | Mechanism |
|---|---|---|
| RPC mode | Kernel | the control stream *is* the kernel's only mode: commands on stdin, events on stdout |
| JSON mode | Kernel | `akern` with prompts on stdin |
| Print mode (`-p`) | Kernel | `akern -p "text"`: only each run's final answer on stdout. `akern-cli "text"` instead renders the event stream through `render.jq` |
| Interactive mode | User (bundled) | `akern-cli`, with or without a first prompt: each line typed is sent (steering while the agent works), results rendered as they arrive. A full-screen TUI would be a separate, larger program against the same stream |
| SDK | — | the protocols are the SDK; any language with pipes and JSON has one |

## Sessions

| Pi feature | Where | Mechanism |
|---|---|---|
| JSONL session files, tree-structured | Kernel | the session log, which is linear ([akern.md](akern.md#session-file)) |
| Per-directory storage, auto-save | User | the `akern-cli` front end picks `~/.akern/sessions/<cwd>/<time>-<id>.jsonl` |
| `--continue`, `--resume`, `--session` | User | `akern-sessions` lists; the `akern-cli` front end passes `-s` |
| `--no-session` | Kernel | omit `-s` |
| `/tree` (branch in place) | — | no equivalent: a session cannot branch in place. A fork with `head -n` is the substitute ([akern.md](akern.md#forks)) |
| `/fork`, `/clone` | User | `akern-fork` writes a session's first lines to a new file; `/clone` is `cp` |
| `/compact` | User | `akern-compact` runs the provider directly for a summary, then appends a `reset`, the summary and the kept copies: directly when the session is unowned, else sent as `append` on the kernel's stdin by the frontend |
| Auto-compaction | User | the front end: on a `run_end` with `context_overflow`, or when `meta.usage` nears the limit, run `akern-compact` and `send` to continue |
| Custom compaction | User | replace or wrap `akern-compact` |
| `/name`, labels | User | `name` and `label` entries |
| `/export` to HTML | User | `akern-export`: a pure function of the JSONL |
| `/share` | User | upload the export |
| Extension state in the session (`appendEntry`) | Kernel | any `x-*` entry via `{"type":"append","entry":{…}}` on stdin (frontends); other programs cannot yet ([roadmap.md](roadmap.md#appending-to-an-owned-session)) |

## Context and prompts

| Pi feature | Where | Mechanism |
|---|---|---|
| `AGENTS.md` / `CLAUDE.md` discovery | User (bundled) | the shipped system-prompt program, `$AKERN_HOME/system/system-prompt`, adds `AGENTS.md`, else `CLAUDE.md`, of each directory from the repository root down to the working directory ([tools.md](tools.md#the-system-prompt-program)) |
| System prompt replace / append | Kernel + User (bundled) | the kernel runs the system-prompt program once at startup and sends its output; `-S file` replaces it with a file. Append: `$AKERN_HOME/system.md`, which the shipped program puts first. Replace: your own `$AKERN_HOME/system-prompt` |
| Prompt templates (`/name`) | User | the `akern-cli` front end expands `prompts/<name>.md` before sending |
| Skills (`SKILL.md`) | User (bundled) | the shipped system-prompt program lists the names and descriptions of `$AKERN_HOME/skills` and `$AKERN_HOME/system/skills` in the system prompt; the model reads them with `cat` |
| `@file` references | User | the frontend turns them into content blocks |
| `!cmd` and `!!cmd` | User | frontend runs the command; `!` sends the output as a prompt, `!!` does not |
| Images | Kernel | content blocks pass through; the frontend creates them |

## Models and providers

| Pi feature | Where | Mechanism |
|---|---|---|
| Anthropic | User (bundled) | `providers/anthropic` |
| OpenAI-compatible (covers most others, local models) | User (bundled) | `providers/openai` |
| Other providers, custom providers | User | one executable each |
| `--model`, `--thinking` | Kernel | `-m provider/model`, `-P effort=high` |
| `/model` switch mid-session | Front end | a new kernel on the same session with `-m`; `akern-acp` restarts its kernel once it has settled |
| Default model setting | User (bundled) | `$AKERN_HOME/model`, one `provider/model` line; `akern-cli` exports it as `AKERN_MODEL` when that is unset |
| Interactive model selector | User (bundled) | `akern-model`: every provider, numbered, with why an unavailable one cannot be used; then the ready one's `--models` ids or a typed id. `akern-cli` runs it when no model is set |
| Model cycling, scoped models, fuzzy match | User | `akern-cli` or frontend feature over `<provider> --models` |
| `--list-models` | User | `akern-models` runs each provider with `--models` |
| `auth login` / `logout` / OAuth subscriptions | User | `akern-login-<provider>`, owned by the provider package |
| Retries, backoff | User (bundled) | inside the provider |
| Usage and cost tracking | Kernel + User | `meta.usage` on every assistant entry; `akern-usage` sums it |
| Provider request/response interception | User | wrap the provider with another provider |

## Customization

| Pi feature | Where | Mechanism |
|---|---|---|
| Extensions | Kernel | tools + providers + commands + frontends |
| Extension-registered commands | User | `bin/akern-<name>` |
| Extension-registered CLI flags | User | environment variables read by the extension; the `akern-cli` front end can map flags to them |
| Keyboard shortcuts, themes, custom rendering, widgets, status line | User | frontend features; the kernel never sees them |
| `ctx.ui.confirm` / `select` / `input` | User | an [`AKERN_ASK`](tools.md#akern_ask) helper, serialized across parallel calls |
| `sendMessage` / `sendUserMessage` from an extension | Kernel + User | `send` on stdin, with any `when`, from the frontend |
| Packages (`install`, `remove`, `update`, `list`) | User | `akern-pkg`: programs onto `PATH`, providers and skills into `~/.akern` |
| Project-local config with trust gate | — | not yet: only `~/.akern` is read ([roadmap.md](roadmap.md#where-settings-live)) |
| Settings files (global, project) | User | read by the `akern-cli` front end and frontends, exported as environment and flags (`-m`, `-P`) |
| Hot reload (`/reload`) | Kernel | tools are found on `PATH` per call and the provider per request; the system prompt is built at startup, so a new skill or `AGENTS.md` needs a new kernel on the same session (`-s`) |

## Things pi leaves to extensions

| Feature | Where | Sketch |
|---|---|---|
| Sub-agents | Kernel + User (bundled) | `akern -p "task"`, run through `shell` with `wait: 0` and `eof: false`; the parent names the child session with `-s`, the kernel counts down the recursion limit, and the answer is the job's stdout. The parent model steers it with `write {"job":"<call id>","content":"line\n"}`; no messages up mid-run, it reports once at the end ([akern.md](akern.md#sub-agents)) |
| Permission gates | User | a shim on `PATH` in front of a program calls `$AKERN_ASK confirm`; non-zero exit denies. Only the gated call waits; the others proceed. A shim of `akern-edit`, or a wrapper `program` for `shell`, gates file changes the same way; reads go through `shell`, so a wrapper `program` for it that sandboxes them gates those |
| Path protection | User | an OS sandbox around the kernel (`bwrap`, `sandbox-exec`, a container) |
| Sandboxed / remote / SSH execution | User | the whole kernel inside `bwrap`, `sandbox-exec` or a container; a shim on `PATH` that runs its program through `ssh host …` |
| MCP | User | a bridge command (`mcp list`, `mcp call SERVER TOOL JSON`) backed by a small daemon that holds the connections, plus a skill that explains it |
| Plan mode | User | a flag file; a wrapping provider appends planning instructions while it exists, and the front end runs the kernel in a read-only sandbox |
| Todos | User | a `todo` command reading and writing a file next to the session, plus a skill |
| Git checkpoints | User | the front end commits to a side ref on each `turn_end` event, keyed by the last `message` event's `id` |

## Pi extension events → this design

| Pi event | Here |
|---|---|
| `input` | the frontend, before it sends |
| `before_agent_start`, `context`, `context_with_system`, `before_provider_request` | a wrapping provider |
| `tool_call` | a shim on `PATH`; an OS sandbox |
| `tool_result` | a shim on `PATH` that filters its program's output (jobs included) |
| `turn_end` | `turn_end` event |
| `agent_before_settle`, `agent_end`, `agent_settled` | `run_end` event; the frontend `send`s to continue |
| `agent_start`, `turn_start`, `message_*`, `tool_execution_*`, `provider_stream_event` | events; observe by reading stdout |
| `message_end` (replace message) | provider wrapper that rewrites the `result` line |
| `before_provider_headers`, `after_provider_response` | inside, or wrapped around, the provider |
| `session_start`, `session_shutdown` | the wrapper script that starts the kernel |
| `session_before_compact` / `_fork` / `_tree` / `_switch` and their after-events | those operations are commands (there is no tree); edit or wrap the command |
| `model_select`, `thinking_level_select` | a new kernel with `-m` / `-P`; each start's `hello` in the session records them |
| `user_bash` | frontend |
| `project_trust` | the `akern-cli` front end |
| `resources_discover` | `PATH` for programs, `~/.akern` for providers and the system-prompt program, which finds skills and context files |

## Known gaps

Honest list of where parity needs more than a script:

1. **A real TUI.** Pi's interactive mode is a large part of its value. Here it
   is a separate project against the control stream. `akern-cli` line mode is a
   placeholder, not a substitute. With concurrency in the kernel, the TUI also
   has more to show: parallel calls, jobs and a tree of sub-agents.
2. **Extension-driven UI.** Pi extensions can draw custom components inside
   the TUI. Here that needs a frontend-specific plugin mechanism; `AKERN_ASK`
   covers only questions and answers.
3. **Provider breadth.** Pi ships many providers and subscription logins.
   Each is an independent executable here, but each still has to be written.
4. **Orphaned processes after `SIGKILL` on macOS.** Process groups plus
   cascading `SIGTERM` cover normal shutdown. A hard kill of a kernel can leave
   a sub-agent's tools running ([process.c](../akern/process.c)).
