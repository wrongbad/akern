# Providers

The API between the kernel and `providers/`: one program per model backend,
run once per request. A provider turns the kernel's request into a model
response; the kernel never speaks HTTP. Stream conventions (JSON lines, unknown
fields ignored, `x-` names): [akern.md](akern.md#conventions).

## 1. Finding and running a provider

| Step | Rule |
|---|---|
| name | `-m provider/model` (or `AKERN_MODEL`, or `set`): the part before the first `/` is the provider, the rest the request's `model` (`""` when there is no `/`) |
| lookup | `$AKERN_HOME/providers/<provider>`, then `$AKERN_HOME/system/providers/<provider>`; the first which must be executable. A name starting with `.` is never run. Not found: `error` `{"where":"provider","message":"no provider for <model> in <home>/providers or <home>/system/providers"}` and the run ends `error` |
| spawn | once per turn, with no arguments, in its own process group, in the kernel's working directory; environment as for every child ([akern.md §1](akern.md#set-for-children)) |
| stdin | the request (§2), then EOF |
| stdout | events and one terminal line (§3); each valid line other than `result` is forwarded to the kernel's stdout as written |
| stderr | each line becomes a `notice` event with `source: provider` |
| `--models` | run by hand or by front ends, not by the kernel: one line per model, `{"id":"…","context":1000000,"max_output":128000}`; `context` and `max_output` are left out when unknown. A provider that cannot be used (no key, no binary) prints an `error` line (§4) instead and exits non-zero, so exit 0 means ready |

Providers are kept off `PATH` on purpose ([design.md](design.md#programs-and-home)).
A provider is a filter, so providers compose: a wrapper can change the request
and `exec` another provider, or filter its output.

## 2. Request

One JSON object on stdin:

```json
{"model":"claude-opus-5-5",
 "system":"You are …",
 "messages":[ … ],
 "tools":[{"name":"exec",…},{"name":"read",…},{"name":"write",…}],
 "params":{"effort":"high","max_tokens":64000}}
```

| Field | Value |
|---|---|
| `model` | string; the part of `provider/model` after the `/` |
| `system` | the system prompt: fixed when `akern` starts, or by a `set` command ([akern.md](akern.md#system-prompt)); `null` when empty |
| `messages` | the context: the `message` of each `message` entry from the session's head back to a root, oldest first ([akern.md §5](akern.md#session-file)); shape in §5 |
| `tools` | the built-in tool definitions, `{name, description, input_schema}` ([tools.md §1](tools.md#1-built-in-tools)) |
| `params` | object from `-P` and `set`; the provider defines its keys and ignores the rest |

## 3. Output

JSON lines on stdout: zero or more events, then exactly one terminal line,
`result` or `error`.

```json
{"type":"text_delta","text":"I'll "}
{"type":"thinking_delta","text":"…"}
{"type":"tool_use_start","id":"tu_01","name":"exec"}
{"type":"tool_use_delta","id":"tu_01","json":"{\"argv"}
{"type":"result","stop":"tool_use","message":{"role":"assistant","content":[…]},
 "usage":{"input":812,"output":64,"cache_read":0,"cache_write":812}}
```

| Line | Fields | Kernel |
|---|---|---|
| `text_delta` | `text` | forwarded; display only |
| `thinking_delta` | `text` | forwarded; display only |
| `tool_use_start` | `id`, `name` | forwarded; display only |
| `tool_use_delta` | `id`, `json` (a fragment of the input text) | forwarded; display only |
| `x-*`, any other type | | forwarded |
| `result` | `stop`, `message`, `usage` | not forwarded; the only line the kernel parses. The last one written wins |
| `error` | `kind`, `message`, `status`; `at` for `invalid_block` | forwarded, and becomes `run_end.error` (§4), unless the kernel omits the block it names |

| `result` field | Value |
|---|---|
| `stop` | `end_turn`, `tool_use`, `max_tokens`, `refusal` or `aborted`. The kernel runs tools only on `tool_use` |
| `message` | the complete assistant message (§5), appended to the session byte for byte. `tool_use` blocks only with `stop: tool_use`: with any other stop they are stored but never answered. An empty `content` array appends nothing |
| `usage` | `{input, output, cache_read, cache_write}` token counts, stored as the entry's `meta.usage` and sent in `turn_end` |

| Kernel rule | Effect |
|---|---|
| a line that is not valid JSON | dropped; `error` `{"where":"provider","message":"invalid line from provider"}` |
| a line longer than `--max-line-bytes` | the provider is killed |
| exit without a `result` | `run_end.reason: error`, unless inbox items are due (the run continues with them). Without an `error` line, the kernel sends `{"where":"provider","message":"provider exited without a result"}` |
| a `result` after an `error` line | the `result` wins |

Exit `0` if and only if a `result` line was written. The kernel reads only the
lines, never the exit status.

## 4. Errors

```json
{"type":"error","kind":"rate_limit","message":"slow down","status":429}
```

| `kind` | Meaning |
|---|---|
| `auth` | credentials missing or refused |
| `rate_limit` | too many requests |
| `overloaded` | the service is overloaded |
| `context_overflow` | the request does not fit the model's context; a front end compacts and retries after `run_end` |
| `invalid_request` | the API rejected the request |
| `invalid_block` | the API rejected an image or document block of the request and said which; `message` is its reason |
| `network` | the connection failed or the stream ended early |
| `other` | anything else |

`status` is the HTTP status, `0` when there was none.

`invalid_block` adds `at`, the block's place in the request's `messages`
(the canonical ones of §2, not the provider's rendering):

```json
{"type":"error","kind":"invalid_block","message":"image exceeds 5 MB maximum","status":400,
 "at":{"message":3,"block":1,"inner":0}}
```

| `at` field | Meaning |
|---|---|
| `message` | index into the request's `messages` |
| `block` | index into that message's `content` |
| `inner` | index into that block's `content` (a `tool_result` or `job_result`); absent for a block of the message itself |

The kernel then swaps that block for a note in the session and sends the
request again ([akern.md: error](akern.md#error)). Apart from that,
the kernel never retries: retrying a model call is the provider's decision,
retrying a run the front end's.

### Abort: SIGTERM

The kernel sends `SIGTERM` when a `now` item preempts the response or the run
is aborted ([kernel.md: driver](kernel.md#driver)). Within 1 second the
provider writes a `result` with `stop: "aborted"` and exits. Its `message` holds only
complete text and thinking blocks that are valid to replay: never a `tool_use`
block, even a complete one, and no thinking block that lacks what the API needs
to accept it again (a signature, for example). The kernel removes any
`tool_use` blocks an aborted result still contains, and sends `SIGKILL` after
`--kill-grace`.

## 5. Messages

The canonical shape is the Anthropic Messages shape: a `role` and an array of
typed content blocks. Other providers translate down from it.

```json
{"role":"user","content":[{"type":"text","text":"list the files"}]}

{"role":"assistant","content":[
  {"type":"thinking","thinking":"","signature":"…"},
  {"type":"text","text":"I'll look."},
  {"type":"tool_use","id":"tu_01","name":"exec","input":{"argv":["ls"]}},
  {"type":"tool_use","id":"tu_02","name":"read","input":{"path":"/p/README.md"}}]}

{"role":"user","content":[
  {"type":"tool_pending","tool_use_id":"tu_01","name":"exec","state":"running","pid":4242,
   "out":"s.jsonl.d/calls/tu_01.out","err":"s.jsonl.d/calls/tu_01.err","stdin_open":false},
  {"type":"tool_result","tool_use_id":"tu_02","is_error":false,
   "content":[{"type":"text","text":"# agent\n…"}]},
  {"type":"text","text":"use the staging config"}]}

{"role":"user","content":[
  {"type":"job_result","tool_use_id":"tu_01","name":"exec","is_error":false,"status":"exit 0","ran":41,
   "content":[{"type":"text","text":"{\"stdout\":\"…\",\"stderr\":\"\",\"exit\":0}"}]}]}
```

### Blocks the kernel reads or writes

| Block | In | Fields |
|---|---|---|
| `tool_use` | assistant | `id`, `name`, `input`: a JSON object; or, when the model's input text was not a JSON object, a string holding that text. Empty input is `{}` |
| `tool_result` | user | `tool_use_id`, `is_error`, `content` (block array) |
| `tool_pending` | user | `tool_use_id`, `name`, `state` (`waiting` for a `--max-processes` slot, or `running`); while its process runs also `pid`, `out`, `err` (its output files) and `stdin_open` |
| `job_result` | user | `tool_use_id`, `name`, `is_error`, `status` (`exit N`, `signal N`, `killed`, `not run`), `ran` (seconds since the program started, rounded; `0` if it never started), `content` (as a `tool_result` would carry it) |
| `text`, `image`, `document` | user | built by the kernel from `send` content and tool results; media as `{"type":"image"\|"document","source":{"type":"base64","media_type":…,"data":…}}`. A `document` may carry `name`, the file's name: `read` sets it; `stdout_type` output has none |

### Ordering

- Each assistant message's calls are answered in the next user message: one
  block per call, in call order, before any other block. A finished or
  cancelled call gets a `tool_result`; one still in progress a `tool_pending`,
  and it is now a job.
- Each `tool_pending` is closed by exactly one `job_result` in a later user
  message, also across a normal shutdown. Only after `SIGKILL` of the kernel
  does one stay open.
- Inbox items delivered at the same moment follow as further blocks of the
  same user message; with no call answers they form a user message of their own.
- Two user messages can be adjacent (a job result delivered at exit, an
  aborted response with no complete text).
- Everything else is opaque: the kernel stores and resends the provider's
  exact bytes.

### What a provider must render

| Input | Provider duty |
|---|---|
| `tool_pending`, `job_result` | facts, not API blocks: render them as something the API accepts (text, or a native asynchronous tool result where an API has one). A `job_result` may arrive after the conversation branched or compacted away its call, so it must not depend on the `tool_use` being in context: `anthropic` renders it as text, never as a `tool_result` |
| adjacent user messages | merge them if the API requires alternation |
| `tool_use` with a string `input` | send `{}`, since APIs take only an object (the kernel already answered it with an error) |
| an image or document the API rejects | send it as is, and map the API's error to `invalid_block` when it names the block, so the session drops it. A provider checks byte counts only: limits whose error names no block (a request's total size), and an image's size where that saves uploading a doomed request; it swaps such a block for a note in the request alone |

Rendering depends only on each message, so earlier messages render the same on
every request and the prompt cache holds (request-wide media limits aside).
The provider owns everything API-specific: credentials, retries, cache
breakpoints, server-tool pauses, format translation and reading the API's
errors. It keeps no table of models or of accepted media types
([design.md](design.md#principles)).

## 6. Shipped providers

**Layout.** `providers/*.c` are the providers, one binary each.
`providers/lib/` holds what they share: `provider.c` (the protocol side:
request, lines, exit status, SIGTERM, the wording of pending calls and job
results), `http.c` (libcurl: SSE streaming POST with retries and timeouts, and
GET), `media.c` (media size limits, per-API), `messages.c` (the Anthropic
Messages format, used by `anthropic` and `claude-code`).

### anthropic

`providers/anthropic.c`, links libcurl (with `openai`). Calls the Messages API
with streaming.

| Input | Use |
|---|---|
| `ANTHROPIC_API_KEY` | required; unset: `error` `auth` |
| `ANTHROPIC_BASE_URL` | default `https://api.anthropic.com`; the request goes to `<base>/v1/messages` |
| model | sent as is. `--models` lists the ids `GET <base>/v1/models` returns, all pages, with `max_input_tokens` as `context` and `max_tokens` as `max_output` when the API gives them |
| `params.max_tokens` | default 64000, sent as is |
| `params.thinking` | default `{"type":"adaptive","display":"summarized"}`; an object is sent as is; `"off"` leaves `thinking` out |
| `params.effort` | `output_config.effort`, default `high`; `"off"` leaves `output_config` out |
| `params.fallbacks` | server-side fallbacks (`fallbacks: "default"` and their beta header), on by default; `"off"` leaves them out |

The defaults suit current models; an older model that rejects one is sent
without it: `-P thinking=off -P effort=off -P fallbacks=off` for
`claude-haiku-4-5`, or `-P 'thinking={"type":"enabled","budget_tokens":8000}'`
for extended thinking.

| Behavior | Rule |
|---|---|
| request | `stream: true`, automatic `cache_control`, `eager_input_streaming` on each tool |
| retries | up to 4 attempts, waiting 1, 2 and 4 s, for network errors, `rate_limit`, `overloaded`, HTTP 5xx and `api_error`; never once an event has been written |
| timeouts | connect 30 s; a stream with no byte for 300 s fails as `network` |
| `stop` | `tool_use`, `max_tokens`, `refusal` as sent; `model_context_window_exceeded` becomes `max_tokens`; anything else `end_turn` |
| fallback | a server-side `fallback` block drops the earlier non-text blocks |
| aborted result | text, signed thinking and redacted thinking blocks; nothing else |
| error kind | from the API error type or HTTP status: 401/403 `auth`, 429 `rate_limit`, 529 `overloaded`, 413 or a "prompt is too long" / "context limit" / "context window" message `context_overflow`; a 400 whose message starts with the position of an image or document (`messages.3.content.1.image.source.base64: …`, or `….content.1.content.0…` inside a `tool_result`) `invalid_block`, with the reason after the position as `message`; other 4xx `invalid_request` |

Rendering:

| Block | Sent as |
|---|---|
| `tool_pending`, `state: running` | `tool_result`: `Still running in the background (pid N, its own process group); its output so far is in OUT and ERR. Its result arrives later as a message.`, plus `Its stdin is open: send it input with write and job "ID".` when `stdin_open` |
| `tool_pending`, any other state | `tool_result`: `Not finished yet; its result arrives later as a message.` |
| `job_result` | a text block `<job call="…" name="…" status="…" ran="Ns">`, the result's text, `</job>`, with the result's non-text blocks after it |
| adjacent user messages | merged; `tool_result` blocks first |
| `document` `name` | its `title` |
| `x-*` blocks, `x-*` fields | dropped; a message without them or a `document` `name` is sent as stored |

Media go as stored, whatever their type; the API judges types, pixel limits and
PDF pages, and its error names the block (§4). Checked here, by byte count only:
an image over 10 MB of base64, and the oldest media first while all media exceed
100 MB, since the request-size error names no block. Each becomes
`[image omitted: REASON. The original is still in the session.]` (`document`
for non-images) in the request only.

### claude-code

`providers/claude-code.c`. Runs each request as one turn of the `claude` CLI
(`claude -p`, found on `PATH`), on the user's Claude Code subscription; usage
counts against the subscription's programmatic credit. It goes through the
binary only and never touches Claude Code's credentials.

| Input | Use |
|---|---|
| model | `-m claude-code/<x>` passes `<x>` to `--model` as is. `--models` lists `fable`, `sonnet`, `opus` and `haiku`, the aliases `claude` documents for the latest model of each family (its `--help` and the Claude Code model configuration docs), without limits: `claude` has no command that lists models. Without `claude` on `PATH`, an `other` error `claude is not on PATH` |
| `system` | `--system-prompt` (`""` when `null`) |
| `params.effort` | `--effort` |
| `params.max_tokens` | `CLAUDE_CODE_MAX_OUTPUT_TOKENS` |

| Behavior | Rule |
|---|---|
| isolation | Claude Code's own tools, CLAUDE.md files, memory, settings, skills, other MCP servers, compaction, advisor and session files are off: `--tools ""`, `--setting-sources ""`, `--disable-slash-commands`, `--strict-mcp-config`, `--no-session-persistence`, `CLAUDE_CODE_DISABLE_CLAUDE_MDS`, `CLAUDE_CODE_DISABLE_AUTO_MEMORY`, `CLAUDE_CODE_DISABLE_ADVISOR_TOOL`, `DISABLE_COMPACT` |
| environment | every `CLAUDE*` variable of a surrounding Claude Code session is removed, except `CLAUDE_CONFIG_DIR`, `CLAUDE_CODE_OAUTH_TOKEN`, `CLAUDE_CODE_USE_BEDROCK`, `CLAUDE_CODE_USE_VERTEX`, `CLAUDE_CODE_USE_FOUNDRY` |
| tools | served to `claude` over MCP by the provider itself, run as `claude-code --mcp TOOLS` (server `akern`), so the model calls them as `mcp__akern__exec` and so on. `--permission-mode dontAsk` refuses every call (a `tools/call` that arrives anyway gets `Not run: akern runs its tools itself.`) and `--max-turns 1` ends the turn after the first response, so `tool_use` blocks come back unrun; the provider strips the prefix from them and adds it to earlier ones in the history |
| history | rendered as `anthropic` renders it, media size limits included, and written as `--input-format stream-json` lines, every user message but the last with `shouldQuery: false`. `claude` appends an assistant message as soon as it reads it but queues user messages, so the provider writes past a queued one only after `claude` acknowledges it with a `result` line; no acknowledgement within 60 s fails with `claude did not take in the conversation` |
| stream | `--include-partial-messages` carries the API's stream events (those of nested agents ignored), read by the same parser as `anthropic` (`providers/lib/messages.c`) |
| failure | no `message_stop`: an `error` whose message is the turn's result text, its first error, or how `claude` exited; `kind` from `api_error_status` and the text as in `anthropic` (a message naming `/login` is `auth`). Never `invalid_block`: for a rejected image or document the result line carries Claude Code's own wording (`Image was too large…`), not the API's, so it names no block |
| abort | `SIGTERM` stops `claude`; the result is `aborted` |

What it loses against `anthropic`:

- the system prompt is not the request's alone: Claude Code adds an identity
  line, an environment block and context reminders such as the date and the
  account's email;
- the prompt cache holds only the system prompt and tools, since those
  reminders sit at the end of each request;
- thinking arrives without its text, so nothing shows as `thinking_delta`
  (the signed blocks are still kept and replayed);
- no server-side fallbacks, no `cache_control`, and no retries but Claude
  Code's own;
- the model knows the tools by their `mcp__akern__` names;
- each request starts a `claude` process, a second or two before the model is
  called;
- an image or document the API rejects fails the run instead of being replaced
  in the session.

### openai

`providers/openai.c`. Calls the Responses API (`POST <base>/responses`) with
streaming, stateless (`store: false`), replaying encrypted reasoning.

| Input | Use |
|---|---|
| `OPENAI_API_KEY` | required; unset: `error` `auth` |
| `OPENAI_BASE_URL` | default `https://api.openai.com/v1` |
| model | sent as is. `--models` lists the ids `GET <base>/models` returns, without limits |
| `system` | `instructions` (left out when `null` or empty) |
| `params.max_tokens` | `max_output_tokens`, only when set |
| `params.effort` | `reasoning.effort`, default `high` |
| `params.summary` | `reasoning.summary`, default `auto`; `"off"` leaves it out |

| Behavior | Rule |
|---|---|
| request | `stream: true`, `store: false`, `include: ["reasoning.encrypted_content"]`, each tool a `function` with `strict: false` |
| retries | as `anthropic`: up to 4 attempts, 1, 2 and 4 s apart, for network errors, `rate_limit`, `overloaded`, HTTP 5xx and `server_error`; never once an event has been written |
| timeouts | connect 30 s; no byte for 300 s fails as `network` |
| events | `output_text` and `refusal` deltas: `text_delta`; reasoning summary and text deltas: `thinking_delta`; a `function_call`: `tool_use_start` (id = `call_id`) and `tool_use_delta` |
| `stop` | a function call: `tool_use`; `incomplete` for `max_output_tokens`: `max_tokens`; for `content_filter`, or any refusal: `refusal`; else `end_turn` |
| usage | `input` = `input_tokens` − cached − cache-written; `cache_read` = `cached_tokens`; `cache_write` = `cache_write_tokens` |
| result | text blocks (with the message's `phase` as `x-openai-phase`), `tool_use` blocks (input parsed from `arguments`, a string when not a JSON object), and each other output item, chiefly reasoning with its `encrypted_content`, as `{"type":"x-openai","item":…}`. A reasoning item is kept only when a kept item follows it |
| aborted result | text only, partial included |
| error kind | 401/403 or `invalid_api_key` `auth`; quota and billing codes `other` (no retry); 413, `context_length_exceeded` or a context-window message `context_overflow`; 429 `rate_limit`; 503 `overloaded`; a 4xx whose `param` names an image or file part (`input[3].content[1]…`, or `input[4].output[1]…` in a `function_call_output`) `invalid_block`, with the API's message; other 4xx `invalid_request`. Read from the error body, a stream `error` event or `response.failed` |

Rendering, message by message:

| Block | Sent as |
|---|---|
| `text` (user) | `input_text` in a user message |
| `image`, `document` | `input_image` with a `data:` URL; `input_file` with `filename` the block's `name`, else `document`, and `file_data` |
| `tool_use` | `function_call` with `call_id` and `arguments`; `"{}"` when `input` is not an object |
| `tool_result` | `function_call_output`: a string for one text block, else a list of input parts |
| `tool_pending` | `function_call_output` with the `anthropic` wording |
| `job_result` | a user message: `input_text` with the `<job …>` text, then its media |
| call answers | before the rest of their user message |
| `text` (assistant) | an assistant message, with `phase` from `x-openai-phase` |
| `x-openai` | its `item`, as received |
| `thinking`, `redacted_thinking`, unknown blocks | dropped |

Media go as stored, whatever their type; the API judges them, and its error
names the part. Checked here, by byte count only: the oldest documents first
while documents exceed 50 MB, and the oldest media first while all media exceed
480 MB, swapped for the same note as in `anthropic`, in the request only.

What it loses: `is_error` on tool results; reasoning in aborted results;
asynchronous tools (`async: true`) are not used, since they change how the
model treats every call, so pending calls and job results are text as in
`anthropic`.

### mock

`providers/mock.c`, for tests. No network; usage is always 1 input and 1
output token. `--models` lists `echo`.

Without `MOCK_SCRIPT`, it answers the last message of the request (its last
text block, and the first text of its first `tool_result` or `job_result`):

| Last text | Response |
|---|---|
| starts with `!` | one assistant message of calls, `stop: tool_use`. The rest is split on `;`; a part `@NAME JSON` is a `tool_use` of `NAME` with that input (`{}` when empty), any other part `exec` with `["sh","-c",PART]`. Ids are `tu_<pid>_<n>` |
| anything else | streamed as 8-byte `text_delta`s, `stop: end_turn`: `The tool said: R` (the first 400 bytes of the result text) plus ` (and you said: T)` when there is text too; otherwise `echo: T` |

With `MOCK_SCRIPT=path`, the file is JSON lines. A group is the lines up to
and including a `result` line. With *n* assistant messages in the request,
the response is group *n* + 1; each line is written as is, except:

| Line | Effect |
|---|---|
| `{"x-sleep":S}` | pause S seconds |
| `{"x-wait":"PATH"}` | wait until PATH exists |

Past the last group: `result` with the text `(script exhausted)`. An unreadable
script: `error` `other`, exit 1. On `SIGTERM` during a pause, it writes an
`aborted` result with the `text_delta` text written so far.
