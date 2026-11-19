# 05 — Pi parity map

The claim in [design.md](design.md) is that every pi feature can be
supplied without changing the kernel. This is the evidence: each feature, the
seam that provides it, and roughly how.

Feature list taken from pi's documentation as of November 2026.

**Where** column:
**K** kernel · **R** reference userland shipped in this repo ·
**U** user extension (buildable today from the contracts, not shipped)

## Core loop and tools

| Pi feature | Where | Mechanism |
|---|---|---|
| Agent loop with streaming | K | `akern-loop` |
| `bash` | K | the built-in `exec` tool: the model runs `["bash","-c",…]` (or any shell it finds) itself |
| `read` | K | the built-in `read` tool: 2000 lines from `offset`, or `limit` lines, at most 100000 bytes, with a note saying where to read on. PNG, JPEG, GIF, WebP and PDF files come back as image or document blocks |
| `write` | K | the built-in `write` tool: atomic replace, or `append`. Nothing passes through a shell, so no quoting and no argv size limit |
| `edit` | R | `akern-edit`: a self-contained C command, run through `exec`, explained by the `files` skill. Arguments pass byte for byte, so multi-line edits need no quoting |
| `grep`, `find`, `ls` | — | the system utilities, through `exec` |
| Tool allowlist / exclude (`--tools`, `-xt`, `-nt`) | U | a restricted `PATH`, or a shim on `PATH` that refuses; `read` and `write` need an OS sandbox |
| Custom tools | U | a command on `PATH` plus `skills/<name>/SKILL.md`; the model runs it through `exec` |
| Tool progress updates | K | stderr chunks → `tool_progress` events |
| Parallel tool calls | K | every `tool_use` in a message starts at once, up to `--max-processes` |
| Abort | K | `abort` command, `SIGINT` |
| Steering (delivered after current tools finish) | K | `steer` = `send` with `when: "turn"`, which is also what a plain line typed while the agent works becomes |
| Follow-up (delivered when the agent finishes) | K | `prompt` = `send` with `when: "idle"` |
| Return queued messages to the editor | K | `run_end.returned` after `abort` |

## Beyond pi: concurrency the kernel provides

Pi leaves these to extensions or does not do them. Here they are kernel
mechanisms, because they are what tests whether the architecture holds.

| Feature | Where | Mechanism |
|---|---|---|
| Immediate steering (interrupt a turn mid-stream or mid-tool) | K | `send` with `when: "now"` interrupts a streaming response; any message wakes a turn waiting on tools, whose unfinished calls are answered pending and keep running as jobs |
| Background tool calls | K | `"wait": false` on the call, or a message arriving while calls run ([concurrency.md](concurrency.md#two-ways-a-call-becomes-a-job)) |
| Job results that wake the agent | K | every finished job's result is queued `turn` and wakes it; only `abort` defers results already queued |
| Model-visible job control | K | a pending call's answer names its pid and output files; `tail`, `kill -TERM -- -PID` through `exec`; waiting is ending the turn. People also have `{"type":"status"}` on stdin |
| Talking to a running program (REPL, sub-agent) | K | `exec` with `stdin_open` keeps the program's stdin open; the model sends it more with `write {"job":"<call id>"}`, and `close` ends it. Steering from another terminal is not supported: whoever started the kernel drives it (use tmux) |
| Model switch without losing background work | K | `set` command |

## Modes

| Pi feature | Where | Mechanism |
|---|---|---|
| RPC mode | K | the control stream *is* the kernel's only mode: commands on stdin, events on stdout |
| JSON mode | K | `akern-loop` with prompts on stdin |
| Print mode (`-p`) | K | `akern-loop -p "text"`: only each run's final answer on stdout. `akern "text"` instead renders the event stream through `render.jq` |
| Interactive mode | R / U | `akern`, with or without a first prompt: each line typed is sent (steering while the agent works), results rendered as they arrive. A full-screen TUI would be a separate, larger program against the same stream |
| SDK | — | the protocols are the SDK; any language with pipes and JSON has one |

## Sessions

| Pi feature | Where | Mechanism |
|---|---|---|
| JSONL session files, tree-structured | K | the session log |
| Per-directory storage, auto-save | R | the `akern` front end picks `~/.akern/sessions/<cwd>/<time>-<id>.jsonl` |
| `--continue`, `--resume`, `--session` | R | `akern-sessions` lists; the `akern` front end passes `-s` |
| `--no-session` | K | omit `-s` |
| `/tree` (branch in place) | R | `akern-tree` appends a `branch` entry (deferred to a boundary if a run is active) |
| `/fork`, `/clone` | R | `akern-fork` copies a chain to a new file |
| `/compact` | R | `akern-compact` runs the provider directly for a summary, then appends a new root: directly when the session is unowned, else sent as `append` on the kernel's stdin by the frontend |
| Auto-compaction | R | the front end: on a `run_end` with `context_overflow`, or when `meta.usage` nears the limit, run `akern-compact` and `send` to continue |
| Custom compaction | U | replace or wrap `akern-compact` |
| `/name`, labels | R | `name` and `label` entries |
| `/export` to HTML | U | `akern-export`: a pure function of the JSONL |
| `/share` | U | upload the export |
| Extension state in the session (`appendEntry`) | K | any `x-*` entry via `{"type":"append","entry":{…}}` on stdin (frontends); other programs cannot yet ([roadmap.md](roadmap.md#settled-since-the-first-draft)) |

## Context and prompts

| Pi feature | Where | Mechanism |
|---|---|---|
| `AGENTS.md` / `CLAUDE.md` discovery | R | the `akern` front end adds found files to the system prompt file it passes with `-S`, or a wrapping provider appends them to `system` |
| System prompt replace / append | K | `$AKERN_HOME/system.md`, `-S file` or `set`; append by editing the file, or in a wrapping provider |
| Prompt templates (`/name`) | R | the `akern` front end expands `prompts/<name>.md` before sending |
| Skills (`SKILL.md`) | R | `make install` lists skill names and descriptions in `$AKERN_HOME/system.md`; the model reads them with `read` |
| `@file` references | R | the frontend turns them into content blocks |
| `!cmd` and `!!cmd` | R | frontend runs the command; `!` sends the output as a prompt, `!!` does not |
| Images | K | content blocks pass through; the frontend creates them |

## Models and providers

| Pi feature | Where | Mechanism |
|---|---|---|
| Anthropic | R | `providers/anthropic` |
| OpenAI-compatible (covers most others, local models) | R (later) | `providers/openai` |
| Other providers, custom providers | U | one executable each |
| `--model`, `--thinking` | K | `-m provider/model`, `-P effort=high` |
| `/model` switch mid-session | K | `set` command; takes effect at the next request |
| Model cycling, scoped models, fuzzy match | U | `akern` or frontend feature over `<provider> --models` |
| `--list-models` | R | `akern-models` runs each provider with `--models` |
| `auth login` / `logout` / OAuth subscriptions | U | `akern-login-<provider>`, owned by the provider package |
| Retries, backoff | R | inside the provider |
| Usage and cost tracking | K + U | `meta.usage` on every assistant entry; `akern-usage` sums it |
| Provider request/response interception | U | wrap the provider with another provider |

## Customization

| Pi feature | Where | Mechanism |
|---|---|---|
| Extensions | K | tools + providers + commands + frontends |
| Extension-registered commands | U | `bin/akern-<name>` |
| Extension-registered CLI flags | U | environment variables read by the extension; the `akern` front end can map flags to them |
| Keyboard shortcuts, themes, custom rendering, widgets, status line | U | frontend features; the kernel never sees them |
| `ctx.ui.confirm` / `select` / `input` | R | `AKERN_ASK` helper, serialized across parallel calls |
| `sendMessage` / `sendUserMessage` from an extension | K + U | `send` on stdin, with any `when`, from the frontend |
| Packages (`install`, `remove`, `update`, `list`) | U | `akern-pkg`: programs onto `PATH`, providers and skills into `~/.akern` |
| Project-local config with trust gate | — | not yet: only `~/.akern` is read ([roadmap.md](roadmap.md) question 11) |
| Settings files (global, project) | R | read by the `akern` front end and frontends, exported as environment and `-o` |
| Hot reload (`/reload`) | K | nothing to do: tools are found on `PATH` per call, the provider and `system.md` per request |

## Things pi leaves to extensions

| Feature | Where | Sketch |
|---|---|---|
| Sub-agents | K + R | `akern-loop -p "task"`, run through `exec` with `wait: false` and `stdin_open: true`; the parent names the child session with `-s`, the kernel counts down the recursion limit, and the answer is the job's stdout. The parent model steers it with `write {"job":"<call id>","content":"line\n"}`; no messages up mid-run, it reports once at the end ([concurrency.md](concurrency.md#sub-agents-recursion-through-the-cli)) |
| Permission gates | U | a shim on `PATH` in front of a program calls `$AKERN_ASK confirm`; non-zero exit denies. Only the gated call waits; the others proceed. `read` and `write` are confined by an OS sandbox instead |
| Path protection | U | an OS sandbox around the kernel (`bwrap`, `sandbox-exec`, a container) |
| Sandboxed / remote / SSH execution | U | the whole kernel inside `bwrap`, `sandbox-exec` or a container; a shim on `PATH` that runs its program through `ssh host …` |
| MCP | U | a bridge command (`mcp list`, `mcp call SERVER TOOL JSON`) backed by a small daemon that holds the connections, plus a skill that explains it |
| Plan mode | U | a flag file; a wrapping provider appends planning instructions while it exists, and the front end runs the kernel in a read-only sandbox |
| Todos | U | a `todo` command reading and writing a file next to the session, plus a skill |
| Git checkpoints | U | the front end commits to a side ref on each `turn_end` event, keyed by the last `message` event's `id` |

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
| `session_before_compact` / `_fork` / `_tree` / `_switch` and their after-events | those operations are commands; edit or wrap the command |
| `model_select`, `thinking_level_select` | `set` command; frontends observe it |
| `user_bash` | frontend |
| `project_trust` | the `akern` front end |
| `resources_discover` | `PATH` for programs, `~/.akern` for providers, skills and `system.md` |

## Known gaps

Honest list of where parity needs more than a script:

1. **A real TUI.** Pi's interactive mode is a large part of its value. Here it
   is a separate project against the control stream. `akern` line mode is a
   placeholder, not a substitute. With concurrency in the kernel, the TUI also
   has more to show: parallel calls, jobs and a tree of sub-agents.
2. **Extension-driven UI.** Pi extensions can draw custom components inside
   the TUI. Here that needs a frontend-specific plugin mechanism; `AKERN_ASK`
   covers only questions and answers.
3. **Provider breadth.** Pi ships many providers and subscription logins.
   Each is an independent executable here, but each still has to be written.
4. **Orphaned processes after `SIGKILL` on macOS.** Process groups plus
   cascading `SIGTERM` cover normal shutdown. A hard kill of a kernel can leave
   a sub-agent's tools running ([concurrency.md](concurrency.md#cancellation-is-a-tree)).
