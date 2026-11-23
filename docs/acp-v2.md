# Agent Client Protocol v2

A design reference for ACP protocol version 2, condensed from the upstream
protocol documentation (the published prose pages, the migration guide and the
JSON Schema). Where this file and the schema disagree, the schema wins. v2 is
in Preview upstream: usable, not frozen. [acp-v1.md](acp-v1.md) describes v1,
which this file assumes you can compare against. Sections marked **(Δ v1)**
changed from v1.

Key words (MUST, SHOULD, MAY) carry their RFC 2119 meaning, as upstream uses
them.

## What changed in one page

1. **The `session/prompt` response no longer ends the turn.** It acknowledges
   that the user message was *inserted* and returns its `messageId`. Foreground
   progress and completion arrive as `state_update` notifications, and the stop
   reason (including `error`) moved there.
2. **Everything is an upsert by ID.** Messages, tool calls, terminals, plans and
   compactions are patched by `(type, id)` with uniform semantics: omitted =
   unchanged, `null` = clear, value = replace, chunks append.
3. **Client file system, Client terminal execution and session modes are gone.**
   Client-side tools are offered as an MCP server the Client passes in
   `mcpServers`. The Agent owns terminals and merely *displays* them.
4. **Capabilities were reorganized.** One `capabilities` + required `info` on
   both sides, session features nested under `capabilities.session`, support
   markers are objects (never booleans), and a required baseline of session
   methods replaces per-method markers.
5. **Everything is extensible.** Nearly every enum and tagged union accepts
   unknown values. `_`-prefixed values are for implementations, the rest are
   reserved for future ACP versions.

Also new: background-work reporting, per-tool-call/terminal cancellation,
structured diffs, idempotent prompt retries, replay cursors with an explicit
`replay_complete` marker, and subagents/notices/compaction/plans with no Client
capability gate.

## Conventions

- Keys are `camelCase`. Discriminator values are `snake_case`.
- Paths MUST be absolute, in the native syntax of the machine the Agent runs on
  (`/home/u/p`, `C:\Users\u\p`, `\\server\share\p`), and are not URIs. Lines are
  1-based.
- Request IDs MUST be strings or integers, never `null`. An error response has
  `id: null` only when the request ID could not be determined. A message with a
  `method` and `id: null` is rejected with `-32600`, not treated as a
  notification. Each side picks its own IDs independently, so the same ID may be
  in flight in both directions. A sender MUST NOT reuse an ID while its request
  is unanswered.
- **No JSON-RPC batches.** A JSON array is rejected with `-32600` and `id: null`.
- Optional fields whose schema allows `null`: omitted ≡ `null`, unless stated.
  Optional fields that disallow `null` (most arrays and objects) are omitted,
  never `null`. Upsert updates are the exception (omit = keep, `null` = clear).
- **Open enums and tagged unions** accept unknown values (a short list of closed
  ones excepted). `_`-prefixed values belong to implementations
  (`_com.example/widget`). Unknown non-underscore values are reserved for future
  ACP. Never mint non-underscore custom values. Receivers SHOULD preserve
  unknown values when storing, replaying, proxying or forwarding, and render
  them generically. A malformed payload for a *known* discriminator MUST NOT
  fall through to the unknown-variant fallback. If a request cannot be
  fulfilled without understanding an unknown variant, reject with `-32602`.
- Receivers MUST ignore unknown object fields.
- **IDs are named for their domain** **(Δ v1)**: `sessionId`, `messageId`,
  `toolCallId`, `terminalId`, `planId`, `compactionId`, `optionId`, `methodId`,
  `configId`, `groupId`. Never a bare `id`.

### Metadata (`_meta`)

`_meta` is `{ [key]: unknown }` on many types. Custom methods start with `_`.
Advertise extensions in a capability object's `_meta` under a reverse-DNS key
(`com.example/name`, methods `_com.example/name`). No custom fields at the root
of spec types. `traceparent`, `tracestate`, `baggage` at the root of `_meta` are
reserved for W3C trace context. No credentials in `_meta`.

**Metadata patches** **(Δ v1)**: the top-level `_meta` of an upsert update is a
*key-level patch*: omitted = unchanged, `null` = clear all, an object patches by
top-level key (a key set to `null` is removed, any other value replaces that
key whole). Metadata nested inside a snapshot, content item, exit status,
command subject or chunk is scoped to that object (omitted ≡ `null`). Per-report
data belongs in the notification's `params._meta`.

### Experimental features

Negotiating `protocolVersion: 2` selects the *baseline*, not the unstable
schema. There is no connection-wide "unstable" flag, and a peer's support must
not be inferred from your own schema, generated types or feature flags. Each
feature defines its negotiation: optional operations needing peer participation
use an advertised capability (omitted/`null` = unsupported). Informational
additions with a safe fallback may be sent without one when the feature says so.

## Transport

stdio is the only standardized transport. Remote (Streamable HTTP/WebSocket) is
a separate proposal, not core v2.

- Client launches the Agent. One JSON-RPC object per line, UTF-8, no embedded
  newlines. Agent logs to stderr. Nothing but ACP messages on stdout/stdin.
- **Shutdown** **(Δ v1)**: the Client closes the Agent's stdin. The Agent treats
  EOF as end of connection (its sessions stop being *active*) and SHOULD exit
  once its work is finished or stopped. The Client MAY then SIGTERM, then SIGKILL.
  Unanswered requests are treated as failed, but may have taken effect.
- **Timing**: after `initialize`, the Agent MAY send requests and notifications
  at any time, including with no Client request pending. Clients MUST handle
  them independently of responses they await.
- **Order**: transports MUST deliver in order, and receivers MUST apply a
  session's `session/update`s in the order received.
- Custom transports MUST preserve framing, ordering and lifecycle. They may add
  reconnect, keepalive or stream resumption provided the ACP messages don't change.

## Methods

Agent methods (Client calls):

| Method | Notes |
|---|---|
| `initialize` | negotiation, see below |
| `auth/login`, `auth/logout` | **(Δ v1)** renamed from `authenticate`/`logout`. Both required iff `authMethods` is non-empty |
| `session/new`, `session/prompt`, `session/list`, `session/resume`, `session/close` | **required baseline** when `capabilities.session` is advertised |
| `session/set_config_option` | required when the Agent offers `configOptions` |
| `session/delete` | optional, `session.delete` |
| `tool_call/cancel`, `terminal/cancel` | new. Required when the Agent advertises `capabilities.cancel` on an item |

Agent notification: `session/cancel`. `session/update` (Agent→Client) is part of
the baseline.

Client methods (Agent calls): `session/request_permission` (baseline),
`elicitation/create` (needs `elicitation.form` and/or `.url`). Client
notification: `elicitation/complete`.

Either side: `$/cancel_request`.

Removed from v1: `session/load`, `session/set_mode`, all `fs/*`, all
Client `terminal/*` execution methods.

Advertising `capabilities.session` commits the Agent to all seven baseline
session methods/notifications: `session/new`, `list`, `resume`, `close`,
`prompt`, `cancel`, `update`. `capabilities.session` may be omitted by Agents
that only serve extension methods.

### Retry classes

When a response is lost (connection ended), the sender cannot tell whether a
request took effect. Each method has a class:

| Class | Rule |
|---|---|
| Idempotent | resend as is: `auth/login`, `auth/logout`, `session/resume`, `session/list`, `session/close`, `session/delete` (treat `-32002` as success), `session/set_config_option` (retry before sending later changes: a late retry overwrites them), `tool_call/cancel`, `terminal/cancel` (a retry after the item is final gets `-32602`), and Agent→Client `session/request_permission`, `elicitation/create` |
| Keyed | `session/prompt` with `idempotencyKey`. A missing or forgotten key means a retry is a *new* submission |
| Duplicating | `session/new`: a retry makes a second session (an unprompted session may be discarded by the Agent) |
| Not retried | notifications and per-connection messages. `initialize` may be repeated only before the first other request. A resent `session/update` chunk would duplicate content, so a Client that missed updates uses `session/resume` replay |

## Initialization

```json
{ "method": "initialize", "params": {
  "protocolVersion": 2,
  "info": { "name": "my-client", "title": "My Client", "version": "1.0.0" },
  "capabilities": { "auth": { "terminal": {} }, "elicitation": { "form": {} } } } }
```

Response: `protocolVersion`, **required** `info`, `capabilities`, `authMethods`
**(Δ v1)** (same field names in both directions; v1 used `clientCapabilities`,
`agentCapabilities`, `clientInfo`, `agentInfo`). `info` is `{name, title?,
version}`. It is self-reported: never use it for security decisions.

**Version negotiation.**
- The Agent reads `protocolVersion` first (integer 0..65535, otherwise `-32602`).
- It selects the **highest supported version ≤ requested**, and responds with
  *that version's* response shape. Never higher than requested.
- None ≤ requested: reject with **`-32022`** `Unsupported protocol version`,
  `data: { requested, supported: [..] }`. The connection stays uninitialized and
  the Client SHOULD close it. A v1 Client sees an ordinary init error.
- If the requested version is newer than the selected one, the Agent MUST ignore
  parameters it cannot parse under the selected version rather than reject.
- The Client MUST check it supports the selected version. If not, it may repeat
  `initialize` with a lower version, else disconnect.
- **Repeat**: before its first other request the Client may repeat `initialize`
  any number of times, waiting for each. The latest *successful* one wins, and a
  failed repeat leaves the previous one in effect. A v1-only Agent may reject a
  repeat.
- Before init completes: the Client MUST NOT send anything but `initialize`
  (Agents answer `-32600`, ignore notifications). Agents MUST NOT send anything.
  A repeated `initialize` after another request is `-32600`.

**Support markers are objects** **(Δ v1)**: `{}` (or an object with fields) =
supported, omitted or `null` = unsupported. So `prompt.image === true` becomes
`capabilities.session.prompt.image != null`.

Agent `capabilities`: `session` (baseline), `session.prompt.{image, audio,
embeddedContext}`, `session.mcp.{stdio, http}` (SSE removed), `session.delete`,
`session.additionalDirectories`, `session.systemPrompt` (with
`mode.override`), and `auth` (a group for auth *extensions* only, not method
availability). Client `capabilities`: `auth.terminal`, `elicitation.{form,url}`.

Gone as capabilities: `loadSession`, `list`/`resume`/`close` markers,
`auth.logout`, `fs`, `terminal`, `session.configOptions.boolean`,
`session.notices`, `session.compaction`, `plan`, `subagents`. Their features are
now baseline: boolean config options, notices, compaction, plans and subagents
need no Client marker, and a Client either implements them or ignores them
(notices and compaction may be ignored). Subagent updates are the exception:
they **cannot** be ignored (see below).

Unstable only: `session.fork`, `archive`, `options`, `status`, `title`,
`promptSuggestions`, `mcp.acp`, `auth.status`, `providers`.

## Authentication

`authMethods[]` entries: `{ methodId, type, name, description? }`. `type` is
required **(Δ v1)**. Stable types: `agent` (Client calls `auth/login {methodId}`)
and `terminal` (Client relaunches the configured Agent program interactively with
the method's `args` appended and `env` applied, then reconnects and
re-initializes. It never sends `auth/login` for it.). Custom types start with `_`.

- `terminal` `env` is an array of `{name, value}` (last duplicate wins), not an
  object. Requires `capabilities.auth.terminal`.
- A non-empty `authMethods` means the Agent MUST implement both `auth/login` and
  `auth/logout`. Empty or omitted: Clients MUST NOT call either.
- Launch safety for terminal auth is unchanged from v1 (no shell, do not apply
  `env` that alters lookup, loading or trust, show args and env first).
- Gated requests fail with `-32000` until authenticated. A failed `auth/login`
  also returns `-32000`. After logout, running sessions may be terminated, kept,
  or fail with auth errors.

## Sessions

### Lifecycle

A session *exists* from `session/new` until the Agent discards it (an Agent may
skip storing a never-prompted session, expire old ones, or `session/delete`).
On a connection a session is **active** from a successful `new`/`resume` until
`close` or end of connection.

- Session-scoped requests need an active session, else `-32002` with
  `data.sessionId`. `session/cancel` for an inactive session is ignored.
- Agents SHOULD NOT discard an active session except via `session/delete`.
- Closing ends it on the connection without discarding it. Whether it stays
  listed and resumable is up to the Agent.
- Resuming an already-active session re-synchronizes it: work continues, and the
  request's MCP servers and roots *replace* the previous ones.
- Multi-connection activity is up to the Agent: it may reject the resume or end
  the other connection's activity as if closed.
- On disconnect the Agent MAY continue or cancel foreground work and MAY insert
  or discard held prompts. A later resume reports *current* activity, not
  whether an unanswered prompt was inserted.

### Setup requests

`session/new` and `session/resume` share environment params **(Δ v1)**:
`cwd` (required, absolute), `additionalDirectories?`, `mcpServers?` (omitted ≡
`[]`, was required in v1), `systemPrompt?`. Resume also takes `replayFrom?`. On
resume the full intended `additionalDirectories` and `mcpServers` must be sent
again: omission activates none and does not restore previous ones.

Responses carry `sessionId` (new), plus `configOptions` and `availableCommands`
**(Δ v1)**. A setup response *replaces* the Client's held lists, including lists
from updates that arrived earlier. An Agent whose session has options or
commands MUST include them. Omitted or `[]` means none. Never `null`. No more
`modes`.

Agents SHOULD put initial state in the `session/new` response rather than
notifications before it, and SHOULD NOT send session-scoped requests before it.
Clients MAY buffer notifications for an unknown `sessionId` until the response,
or drop them. A new session is `idle` unless a `state_update` says otherwise
(never overwritten by the response). An Agent whose new session is not idle MUST
send `state_update` *after* the response.

`systemPrompt { mode: append|override, content: [text] }` is unchanged from v1:
needs `session.systemPrompt`, `override` needs `mode.override`, text blocks only
and non-empty, errors are `-32602`, and nothing is created on error. On resume,
Clients SHOULD resend the one used at creation.

`cwd` is the primary root and base for relative paths. The root set
`[cwd, ...additionalDirectories]` is a scope boundary, not a sandbox. Agents
should not take model output or replayed history as access outside it, and
enforcement should check the resolved path at time of use and fail closed.

### Resume, replay and current state **(Δ v1)**

`session/resume { sessionId, cwd, ..., replayFrom? }` replaces both `load` and
`resume`:

- **`replayFrom` omitted/`null`**: no history replay. The Agent restores
  context, reconnects MCP servers, sends the *current-state snapshot*, and
  responds. It does not resynchronize messages or foreground tool calls (a
  message still streaming simply continues live).
- **`replayFrom: {type:"start"}`**: replay all retained history as ordinary
  `session/update`s, then `replay_complete`, then current state, then the
  response. Cursors are inclusive and a tagged union (custom/future types are
  parseable so intermediaries can forward them). An unsupported or malformed
  cursor gets `-32602`. Never silently substitute another mode.

**Replay contents**: same updates and semantics as live (messages, tool calls,
display terminals, plans, compactions). The Agent may send an entity's current
state (a whole message, a terminal `output` snapshot) rather than every chunk.
Replayed finished tool calls SHOULD have a final status. Not replayed:
historical `state_update`, `usage_update`, `session_info_update`,
`config_option_update`, `available_commands_update`, notices, and pending
permission requests or elicitations.

- User messages inserted by `session/prompt` MUST be replayed with the ID that
  prompt returned, across restarts. Other IDs (agent, thought, tool call,
  terminal, plan, compaction) SHOULD be preserved but Clients MUST NOT depend
  on that or use them to dedupe replay against earlier history. Agents need not
  persist every message (live-only messages and local commands may be absent).
  Replaying a command does not run it again.
- **Chunk reset rule**: if an entity is replayed as chunks, first send its
  update with the chunked field emptied (`content: []`, or `output: {data:""}`
  for terminals, `summary: []` for compactions), so chunks don't append to
  content the Client already holds. It only resets that ID. It cannot clear the
  same message previously delivered under a different ID.
- Replay also applies `_meta` as a patch: remove stale keys with `null` or send
  `_meta: null` first.
- **`replay_complete`**: exactly once, after the last replayed update and before
  the response, even for an empty replay. Never when `replayFrom` is omitted, or
  for `session/new`, and never when replay could not finish. From request
  receipt until it is sent, every update for that session MUST be replayed
  history. The Agent holds anything else (streaming chunks, `notice`,
  `config_option_update`, `available_commands_update`, current-state sync) and
  Agent requests (permissions, elicitations) until after it. It marks the *end*
  of replay, not the start, since a live update sent before the request was
  received can arrive right before it. A replay that ends with neither the
  marker nor a success response is truncated. It does not mean the Agent kept
  all history (SHOULD report known gaps with a notice after it). It does not
  make the session active: that happens with the successful response, so
  `session/prompt` before that is `-32002`.
- **Current-state synchronization**: every successful resume MUST be preceded by
  a `state_update` snapshot (after any replay). Resume does not imply idle, and
  Agents MUST NOT replay historical states as current activity. The snapshot's
  `background` list names terminals, tool calls and children, so the Agent first
  sends a current `terminal_update`/`tool_call_update`/`subagent_update` for each.
- Child sessions: see Subagents.

### Closing and deleting

`session/close { sessionId }` is required and idempotent. It cancels ongoing
work like `session/cancel` (final updates and the idle `cancelled` state come
*before* the close response) and prompts not yet inserted get `-32800`. Unlike
cancel, it also stops background work. Treat `-32002` as success.
`session/delete` (optional) also cancels work, as if closed when active. Later
requests, including `resume`, fail with `-32002`.

### Listing

`session/list { cwd?, cursor? }` returns `{sessions, nextCursor?}`: opaque
cursors, missing `nextCursor` is the end, invalid cursor is `-32602`, empty
result is an empty array. `SessionInfo`: `sessionId`, `cwd`,
`additionalDirectories?`, `title?`, `updatedAt?` (`date-time`), `_meta?`.
Children are not listed. `session_info_update` patches `title`, `updatedAt`
and `_meta` (key-level).

### MCP servers **(Δ v1)**

Every config has a required `type`. SSE is removed.

- `stdio`: `{type, name, command (absolute, else -32602), args?, env?[{name,value}]}`
  (needs `session.mcp.stdio`, now explicit).
- `http`: `{type:"http", name, url, headers?[{name,value}]}` (needs `session.mcp.http`).
- `name` MUST be unique within `mcpServers` (duplicates are `-32602`). Unadvertised
  or unknown transports are `-32602`.
- Same credential rules as v1: `env` and `headers` values are secrets, launch
  without a shell, keep out of updates, errors, `_meta`, listings, history,
  model context and logs, send headers only to the configured origin, do not
  take a stdio server from workspace files or Agent output without approval.
- **This is how a Client offers file or command tools to an Agent**: it runs an
  MCP server and passes it in.

## The prompt lifecycle **(Δ v1)**

### Prompt

`session/prompt { sessionId, prompt: ContentBlock[] (≥1), idempotencyKey? }`.
The Agent MUST respond once the message has been **inserted** into the ACP
conversation, without waiting for foreground processing:

```json
{ "result": { "messageId": "msg_user_8f7a1" } }
```

Merely receiving, queueing or ID-ing the input does not justify success. The
Agent owns the ID (Clients do not generate message IDs). The response and the
`user_message` update (or `user_message_chunk`s) describe one insertion and may
arrive in either order. Clients reconcile by `(sessionId, messageId)`, MUST NOT
match by content, and MUST NOT take a successful response to mean the session is
idle. SDKs must keep processing notifications after the RPC completes.

- **Content**: an Agent MUST reject (`-32602`, not inserted) a prompt with a
  content type that is neither baseline (`text`, `resource_link`) nor advertised.
  v1 only constrained Clients.
- **Prompts during active work**: Clients MAY send a prompt in any state. The
  Agent MUST NOT reject it just because work is active. It inserts it right away
  or holds it (response pending until insertion), in arrival order. What
  insertion means (steer vs queue) is the Agent's call. A held prompt is dropped
  by `session/cancel` or `session/close` (answered `-32800`), or best-effort by
  `$/cancel_request`. An inserted prompt cannot be withdrawn: stop its work with
  `session/cancel`.
- **Never inserted**: reject with a JSON-RPC error and emit **no** user-message
  updates for that submission. Foreground-state reporting is independent: a
  failed foreground hook may need both an error response and an idle update. An
  error response to `session/prompt` therefore *always* means "not inserted".
  Pre-insertion request cancellation uses `-32800`. After insertion `-32800` can
  no longer answer the prompt.
- **Local slash commands** go through the same lifecycle (`running` → `idle`,
  `end_turn`, even with no model call). User messages, including commands, MUST
  be reported live but need not be retained.

### Idempotency key

`idempotencyKey`: opaque, case-sensitive, 1–255 chars, else `-32602` (never
treated as omitted). The Client generates one per *submission* (UUID or similar
entropy, not derived from content, no secrets), stores it with the pending
input, reuses it on every retry including after restart, and never reuses it for
a different prompt. Scoped to the session. Agents that advertise `session` MUST
support it.

| Agent has recorded | Action |
|---|---|
| nothing | handle normally and record key with the inserted message |
| inserted message with this key | do not insert, return original `messageId` (whether to continue unfinished work is up to the Agent) |
| held, not yet inserted | answer both requests together with the same outcome |

Errors leave the key unused. A recorded key with a different prompt gets either
the retry answer or `-32602` (comparison not required, and the key stays bound
to the first prompt). Never echo keys in updates, lists or replay. Retention: as
long as the Agent retains the message (across restarts), or, for live-only
messages, while the session stays active on a connection. After that it may
forget. A forgotten key means a retry is a new submission, even immediately
after reconnecting. Agents SHOULD store the key in the same step as the message
(for example derive the message ID from it with UUIDv5). If a duplicate would
matter and cannot be ruled out, the Client SHOULD NOT auto-resend. Tell the user
it may have run. Intermediaries forward the key unchanged.

### Foreground vs background work

- **Foreground work**: the Agent's active response or workflow, including model
  processing, tool execution and waiting on user action needed to continue.
  Prompt-driven or Agent-initiated (pursuing a goal, acting on a finished
  background task) are treated identically and need no prompt or synthetic
  user message.
- **Background activity** runs independently: detached tasks and their progress
  or completion, session metadata updates. It does not itself start foreground
  work. If the Agent starts a response to act on a background result, *that* is
  foreground.

### `state_update`

A snapshot of aggregate foreground activity (not per prompt, call or request):
`{ state, stopReason?, error?, background? }`.

| state | Meaning |
|---|---|
| `running` | foreground work in progress |
| `requires_action` | the foreground workflow *as a whole* is blocked on user action |
| `idle` | no foreground work. Background may continue |
| `unknown` | Agent cannot currently determine activity (for example a lost child feed) |

Transitions:

| From | When | Report |
|---|---|---|
| `idle` | foreground work starts | MUST `running` (or `requires_action` if it starts blocked, no artificial `running` first) |
| `running` | work cannot progress without the user | SHOULD `requires_action` |
| `requires_action` | progress resumes | MUST `running` |
| `running`/`requires_action` | all foreground activity ends | MUST `idle` (even between immediately consecutive turns) |
| any | only background change, or an unrelated request | unchanged |
| any | same state repeated | idempotent, not new work or a new completion |

- Concurrent requests: opening or resolving one does not itself transition. The
  state stays `running` while other foreground work can progress. Once
  `requires_action` is reported the Agent MUST report `running` when progress
  resumes, even with requests still outstanding. Answering one request does not
  imply `running`. Late answers for ended work SHOULD be ignored.
- Completing one prompt or command while other foreground work continues needs no
  idle boundary.
- A repeated state may carry new info (for example an updated `background` list).
  Applying it is not a transition.
- **Unknown/unrecognized states**: Clients MUST NOT treat them as `idle` or keep
  showing the previous state as confirmed. Show a generic busy state. On
  `unknown`, stop presenting earlier `running`/`requires_action` as confirmed
  (the last-known value may be kept for context). `unknown` is not an outcome,
  closure or cancellation, resolves no pending requests and revokes no
  capabilities (the Agent updates those explicitly).
- A permission request or elicitation alone changes nothing. If it belongs only
  to independent background work, the session may stay `idle`.

### Stop reasons

On the idle update: `end_turn`, `max_tokens`, `max_turn_requests`, `refusal`,
`cancelled`, and **`error`** **(Δ v1)**. Open enum, and the idle update's stop
reason is a union tagged by `stopReason` and flattened into the update, so a
reason can carry fields (`error` carries `error`). Missing, `null` or unknown
stop reason: still recognize idle, and infer neither success nor failure. Keep
unknown reasons with their fields.

- `refusal` lost its v1 meaning of "drop the prompt from the next request". It
  names no message, so Clients do not remove or mark messages.
- **Failures**: foreground work that ends on a failure MUST go idle and SHOULD
  use `stopReason: "error"` with `error: {code, message, data?}`. First give each
  unfinished tool call of the failed work a final status (`failed` for calls
  that did not complete). Listed background work keeps running. The failure
  does not undo insertion or side effects already produced (so resubmitting as
  new input can repeat them). `error` stop reason is *only* for failures that end
  foreground work: pre-insertion failures are error responses, recovered
  failures use a notice or a `failed` tool call and don't change state, errors
  raised while aborting cancelled work are `cancelled`, and failures in
  background activity while idle are not a stop reason. Lost connections are
  detected by Clients, not reported.

### Cancellation

`session/cancel { sessionId }` is a notification. It covers foreground work and
prompts received before it but not yet inserted (answered `-32800`). A prompt
received after it is not covered, and an already-inserted prompt still succeeds
with its `messageId`.

- The Agent SHOULD stop model requests and tool calls ASAP. When it stops active
  foreground work it MUST send that work's pending updates, then idle with
  `stopReason: "cancelled"`, and MUST do so before starting *new* foreground work
  in the session (including Agent-initiated). It does not wait for listed
  background work or for answers to abandoned requests. If foreground work had
  already ended, its stop reason stands and no extra idle is required. Abort
  exceptions from model libraries MUST be reported as `cancelled`, not `error`.
- It MAY insert and acknowledge a later prompt during cancellation (if inserting
  would start work immediately, hold it with the response pending). It MUST NOT
  reject it only because cancellation is in progress.
- Work in the latest `background` list keeps running. To stop one item use
  `tool_call/cancel` / `terminal/cancel` while it advertises `capabilities.cancel`.
  `session/close` stops background work too.
- **Permission requests** **(Δ v1)**: the Agent MUST stop waiting for the
  cancelled work's pending `session/request_permission`s, treat them as not
  approved, and cancel each with `$/cancel_request` (only the Agent knows which
  requests belong to the cancelled work). A Client getting `$/cancel_request`
  for one SHOULD dismiss the prompt and answer `cancelled`. It MAY answer a
  request it knows belongs to the cancelled work without waiting. Clients no
  longer answer *all* pending permissions on `session/cancel` as in v1. Requests
  for listed background work are unaffected. For elicitations, see Elicitation.
- Client SHOULD mark unfinished tool calls of the cancelled work `cancelled`
  locally (skipping those in the latest `background` list or referencing a
  listed terminal). Agent updates override. Clients keep accepting updates after
  cancel.
- After cancellation completes, ignore late responses and external completions
  rather than resuming the work. A repeated or resume snapshot carrying an old
  `cancelled` stop reason does not cancel a current interaction.

### Background work

`state_update` (all four states) may carry `background`: a list of what runs
independently of foreground work:

```json
"background": [ {"type":"terminal","terminalId":"t3"},
                {"type":"tool_call","toolCallId":"c12"},
                {"type":"subagent","sessionId":"s_child"} ]
```

- A snapshot: a list replaces the previous one, `[]` means nothing is running.
  **Omitted or `null` means "not reported", not "nothing running".** An Agent
  that reports background work MUST include the list on *every* `state_update`.
  Re-send the state when the list changes (not a transition or a new
  completion).
- Announce before listing (`terminal_update`/`tool_call_update`/`subagent_update`
  sent before the first snapshot naming it). Finalize before removing
  (`exitStatus` or final status first). Work with no process stays on its tool
  call (keep it `pending`/`in_progress`/`paused` and listed rather than giving it
  a final status). The Agent may start its own tool call for work that has none,
  and a final tool call is never reopened (start a new one).
- Only work a user can reason about is listed (not housekeeping like memory
  consolidation, unless it is made visible as a tool call). Only background is
  listed, and only once: a terminal or call that foreground waits on is
  foreground. Process-backed work is listed as the terminal, never also its tool
  call. Unknown item types still count as running work. A background child is
  listed while its state is not `idle` or its own list is non-empty or unreported.
- If finished background work will start follow-up foreground work and the Agent
  knows, it SHOULD report `running` in the same snapshot that removes the item
  or earlier. Otherwise Clients cover the gap with a short grace period.
- Clients use it for: whether the session is quiet (`idle` and `[]`; only
  `pending`/`paused` calls listed means nothing is running now, and Clients that
  free resources when quiet SHOULD wait a grace period), what `session/close`
  would stop, and which unfinished items are live. While idle a Client MAY treat
  an unfinished item absent from a *reported* list as abandoned, never when it is
  not reported.

## Updates

`session/update { sessionId, update }` with `update.sessionUpdate`:

| `sessionUpdate` | Notes |
|---|---|
| `user_message`, `agent_message`, `agent_thought` | whole-message upserts, `content` is an *array* |
| `user_message_chunk`, `agent_message_chunk`, `agent_thought_chunk` | one content block, appends. `messageId` now required |
| `session_message`, `session_message_chunk` | messages between sessions (subagents) |
| `tool_call_update`, `tool_call_content_chunk` | tool upsert / append one content item. `tool_call` is gone |
| `terminal_update`, `terminal_output_chunk` | Agent-owned display terminal |
| `plan_update`, `plan_removed` | identified plans. `plan` is gone |
| `subagent_update` | announces/patches a child session |
| `state_update` | foreground state (see above) |
| `available_commands_update`, `config_option_update` | each replaces the complete list (`[]` clears) |
| `session_info_update`, `usage_update` | patches. `usage_update.cost` omitted = unchanged, `null` = clear |
| `notice` | live user-facing notice |
| `compaction_update`, `compaction_summary_chunk` | compaction entity |
| `replay_complete` | end of a requested replay |

`current_mode_update` is gone (modes are config options).

### Entity IDs and upsert semantics

Entities are identified by **type + ID**, not ID alone: user message, agent
message, agent thought and session-directed message are separate types even
though they share `messageId`. A thought and a response derived from the same
model output may share a `messageId` and remain two entities. Same for
`toolCallId`, `terminalId`, `planId`, `compactionId`. The Agent MUST NOT reuse
an ID for a different entity of the same type in a session. Clients MUST key by
`(type, id)`.

- An update or chunk for an unseen ID *creates* the entity with defaults and
  applies, so a chunk can arrive before any whole update, and Clients MUST NOT
  drop it. Exception: Clients MUST ignore `plan_removed` for an unknown `planId`.
- An entity's first update/chunk fixes its timeline position in receive order,
  after previously received content and before content first received later.
  Later updates apply in place without moving it, even after `idle`.
  Entities may interleave. Clients showing an entity outside the timeline (a plan
  panel) may ignore position.
- **Patch semantics**: omitted = unchanged, `null` = clear, concrete value =
  replace (arrays replaced whole). Chunks *append*. A concrete whole-field
  update replaces everything accumulated so far (chunks included), and later
  chunks append to the replacement.
- Messages: `agent_message` `[A]` then chunk `B` renders `[A, B]`. Then
  `agent_message` `[C]` resets to `[C]`. `content: null` or `[]` clears.
  Adjacent text blocks form one continuous text (Clients SHOULD join without a
  separator).

## Content blocks

Same five types as v1 (`text`, `image`, `audio`, `resource_link`, `resource`),
realigned with MCP 2026-07-28 and a superset of MCP's `ContentBlock`. `type` is
an open discriminator. `resource_link` gains `icons[]` (`src`, `mimeType?`,
`sizes?`, `theme?`: light|dark). Annotations and icons take `_meta`.
Schema tightening: base64 fields have `contentEncoding: "base64"`, URIs
`format: "uri"`, `updatedAt` `format: "date-time"`, `annotations.priority`
0..1, `prompt`/`systemPrompt.content` ≥1 block, `resource_link.size` unsigned,
tool location `line` ≥1.

Text is Markdown, and this also applies to plan entry `content` and the
`description` of auth methods, commands, config options (and their values),
permission requests and subagents. Other user-visible strings (tool call
`title`, notices, etc.) are plain text.

Same rendering/URI security posture as v1: Agent content is untrusted, raw HTML
disabled or sanitized, no auto-fetch of remote URIs, no scripts in SVG, no
`file:` URIs outside the root set, show full link targets, reveal bidi and
zero-width characters in commands, paths and URLs.

## Tool calls **(Δ v1)**

**One upsert: `tool_call_update`.** The first update for an unseen `toolCallId`
creates it. Only `toolCallId` is required, and the Agent SHOULD include `title`
on the first report. Fields: `name?` (informational, not a capability or
authorization), `title`, `kind`, `status`, `content[]`, `locations[]`,
`rawInput`, `rawOutput`, `capabilities?`, `_meta`. Omitted = unchanged, `null` =
clear (so `name: null` is expressible in v2 but not v1), arrays replaced whole,
top-level `_meta` is a key-level patch stored on the call.

- `kind` (open): `read`, `edit`, `delete`, `move`, `search`, `execute`, `think`,
  `fetch`, `switch_mode` (now means changing the `mode`-category config option),
  `other`.
- `status` (open): `pending`, `in_progress`, **`paused`**, `completed`,
  `failed`, **`cancelled`**. `pending` = not started (input streaming, awaiting
  approval, or scheduled for later). `paused` = started, not running, will not
  continue until resumed. An Agent never moves a call back to `pending`
  (including `status: null`). `in_progress` and `paused` may alternate any
  number of times. `completed`/`failed`/`cancelled` are **final**: further work
  goes on a new tool call. When foreground work ends, the Agent SHOULD give each
  unfinished call of that work a final status (calls in `background` excepted).
  Custom statuses start with `_`. A call with an unrecognized status is shown
  as in progress.
- `tool_call_content_chunk { toolCallId, content: ToolCallContent }` appends one
  item. A `tool_call_update` with `content` replaces accumulated content, and
  chunks append after.
- **Content items** (open union): `content` (wraps a content block), `diff`,
  `terminal { terminalId }` (display anchor), `session { sessionId }`
  (display-only link to another session, no capability needed in v2,
  `_meta` scoped to the item).
- `locations[] { path, line? }`: display-only "follow the agent". `line` ≥1
  (was `0` allowed). Do not auto-open outside the root set.

### Cancelling a tool call or terminal

Items opt in per item with `capabilities: { cancel: {} }` on
`tool_call_update` / `terminal_update`. This is a nullable patch: an object
replaces the whole previous value, `null` clears, `cancel` omitted or `null`
means unsupported. A `pending` or `paused` call may advertise it (for example to
delete a scheduled job). The Agent SHOULD stop advertising it once final.

- `tool_call/cancel { sessionId, toolCallId }` / `terminal/cancel { sessionId,
  terminalId }`. The Client MUST NOT send unless the item currently advertises
  `cancel`. `sessionId` is the session that reports the item. For a subagent's
  item that is the child, and the item's own `cancel` enables the request even
  if the child doesn't advertise `cancel`.
- Response is `{}` once the Agent *accepted* it. It does **not** mean the work
  stopped. Confirmation is a `tool_call_update` to a final status (normally
  `cancelled`, but `completed`/`failed` if it finished first). Unknown, already
  final or not-cancellable items get `-32602` (including a retry after the item
  became final). These methods have no `initialize` capability, so an Agent that
  implements them answers `-32602` rather than `-32601` for a non-cancellable
  item.
- It does not change the foreground state by itself, and works for foreground
  items too (stop one long step without cancelling the rest).

### Diffs **(Δ v1)**

`{ type:"diff", changes[], patch? }` replaces `{path, oldText, newText}`.

- `changes` (≥1) is authoritative: `{ operation: add|delete|modify|move|copy,
  path, oldPath? (move/copy), fileType?: text|binary|directory|symlink,
  mimeType? }`. Clients build trees and summaries from it without parsing
  patches.
- `patch { format: "git_patch", text }` is optional renderable text for some or
  all changes and MUST be consistent with `changes`. It uses `diff --git`
  sections in `git diff -p` format, but **every path is absolute** (including in
  `rename from/to`, `copy from/to`), with no `a/` `b/` prefix, `/dev/null` for
  the missing side, Git's quoting for odd paths, and no commit metadata or email
  envelope. Agents SHOULD provide it when feasible, and Clients MUST handle it
  being absent (binary, symlink retarget).
- No mechanical mapping back to old/new text. A v1-style Agent that only knows an
  edit's snippet must either read the file *before* the tool can run (somewhere
  the tool waits, not after input completes, since an auto-approved edit may
  have already run) or send no diff until the change is done (`locations` can
  still point at the file).

### Agent-owned display terminals **(Δ v1)**

`terminal` content is only a reference. State comes from `terminal_update`
(upsert by `terminalId`): `command?`, `cwd?` (absolute), `size?{cols,rows ≥1}`,
`output?{data (base64), _meta?}`, `exitStatus?{exitCode?, signal?, _meta?}`,
`capabilities?`, `_meta`. `terminalId` is stable, never reused, independent of
`toolCallId`, and the reference and updates may arrive in either order (retain
state for a first-seen ID).

- `output` is an authoritative *replacement snapshot* of all bytes to retain.
  Never splice with previous bytes.
- `terminal_output_chunk { terminalId, data }`: each `data` is independently
  base64. Decode each separately and append bytes in order. Do **not** join the
  encoded strings. Chunk boundaries may split UTF-8 and escape sequences, so
  keep parser state. A later snapshot replaces everything and chunks append to
  it. A chunk for an unseen ID creates the terminal.
- A concrete `exitStatus` marks exit (`signal` uses platform names such as
  `SIGTERM`). Terminal exit and tool-call status are independent. Agents SHOULD
  send all output before `exitStatus`. `exitCode` is signed.
- It is display-only: no input, resize, interrupt, wait or release. The only
  control is opt-in `terminal/cancel`. Clients need only provide a safe sanitized
  transcript fallback, and SHOULD neutralize control sequences (clipboard
  writes, query responses, links) if they render in an emulator.

## Permission requests **(Δ v1)**

```json
{ "method": "session/request_permission", "params": {
  "sessionId": "s", "title": "Run this script?", "description": "…",
  "subject": { "type": "tool_call", "toolCall": { "toolCallId": "c1" } },
  "options": [ {"optionId":"allow","name":"Allow once","kind":"allow_once"} ] } }
```

- `title` required (prompt copy, never updates a tool call's title).
  `description?` (Markdown). `options` ≥1, ids unique.
- `subject?` open tagged union: `tool_call { toolCall: ToolCallUpdate }` (applied
  to stored state exactly like a `tool_call_update`, in receive order, before
  showing the prompt) or `command { command, cwd (absolute), toolCallId?,
  terminalId?, _meta? }`: self-contained, the Agent executes it and approval
  never asks the Client to. Unknown subjects: preserve when proxying, render
  generically or decline by policy with `cancelled`. A request for listed
  background work SHOULD reference the item and the Agent MUST list it first.
- Outcome: `{outcome:"cancelled"}` (also for user dismissal or policy decline, so
  it no longer implies the turn was cancelled, and it doesn't mean foreground
  work is ending) or `{outcome:"selected", optionId}`. The Agent MUST match
  `optionId` to an offered option. A selected `reject_*` means don't proceed.
  An unoffered ID, or an unknown kind or outcome, is never approval. The Agent
  owns remembered decisions (`allow_always`/`reject_always`): it stores them,
  decides scope and lifetime, and the option `name` states the scope.
- Same advisory-permission and security guidance as v1 (describe from
  kind/locations/content, show commands and paths in full, don't auto-approve
  on `title`/`name` alone, re-request if the command or target changes, don't
  silently reduce oversight by switching mode).
- State: blocked foreground → `requires_action`. See `state_update` rules.

## Plans **(Δ v1)**

Only `plan_update { plan }` and `plan_removed { planId }`, no capability.
`plan` is `{type:"items", planId, entries[{content, priority: high|medium|low,
status: pending|in_progress|completed|cancelled}]}` (priority and status are
open) or `{type:"markdown", planId, content (complete plan), path? (absolute)}`.
Each update replaces the whole plan with that ID (even across types, even to an
unknown type). `entries: []` empties a checklist (Clients may hide it).
`plan_removed` for cancelled plans, ignored when unknown. A finished plan need
not be removed.

## Slash commands **(Δ v1)**

`availableCommands[]` in setup responses and `available_commands_update`
(complete replacement, `[]` clears). Entries are tagged: `{type:"command", name,
description, input?: {type:"text", hint}}`. `name`s unique. Unknown command
types: preserve when proxying, and a Client that doesn't understand one ignores
it and MUST NOT show it as a slash command. Unknown input types: fall back to
plain text or hide the hint. Running a command is still a prompt whose text
starts with `/name`.

## Config options (replace modes) **(Δ v1)**

`configOptions[]`: `{ configId, name, description?, category?, type,
currentValue, options? }`. `type` is `select` (string option IDs, flat or groups
`{groupId, name, options}`) or `boolean`, no capability needed.
`configId`/values/`groupId` unique. Categories `mode`, `model`, `model_config`,
`thought_level` (`_` = custom): UX only, tolerate unknown. List a `select`
`thought_level` from lowest to highest. Order is Agent priority.
`currentValue` must be workable if the Client ignores the option.

`session/set_config_option { sessionId, configId, type: "id"|"boolean", value }`
returns the **complete** options list (a change may alter others). Errors:
unknown `configId`, invalid value, or mismatched/unsupported `type` are
`-32602`. Agent-initiated changes use `config_option_update` (complete list).
Mode-like state is just a `category:"mode"` option, with no `modes` field and no
`session/set_mode`.

## Elicitation

Largely unchanged. Adapted from MCP elicitation. Client advertises
`elicitation.{form,url}` (`{}` advertises none, unlike MCP).

- `elicitation/create` needs `mode` (no default), `message`, and exactly one of
  `sessionId` (session-scoped, optional `toolCallId`) or `requestId`. Session
  scoped requests follow the foreground-state contract (a blocking one means
  `requires_action`).
- **form**: flat `requestedSchema` (`type:"object"`, `properties`) of primitives
  and enums. **`pattern` removed on strings** **(Δ v1)** (keep `minLength`,
  `maxLength`, `format`, `default`). Agents validate themselves, Clients don't
  evaluate patterns. Not for secrets. No form fallback for URL mode.
- **url**: `elicitationId`, `url`. `accept` means consent to open, not
  completion. Credentials never flow back over ACP. Agent MAY send
  `elicitation/complete`. Show the full URL, get consent, never prefetch, open in
  a context the model cannot inspect, highlight domain, warn on Punycode.
- Response `action`: `accept` (`content` optional, may be any JSON value),
  `decline`, `cancel`. **An unknown action MUST be handled as `cancel`**
  **(Δ v1)**.
- Errors **(Δ v1)**: a Client with no `elicitation` capability answers
  `-32601`. A Client that advertised `elicitation` but not the requested mode
  answers `-32602`.

## Subagents **(Δ v1)**

Child sessions owned by the Agent and only observed by the Client. Optional for
Agents. In v2 there is **no Client capability**, so every Client MUST handle
`subagent_update`, `session_message` and `session_message_chunk`: register the
child, attribute its updates and requests to it, never drop them as "unknown
session", and show session messages as inter-session messages (not user
messages).

- **Announce** with `subagent_update { sessionId (child), title?, description?,
  capabilities?{cancel?}, state?, cwd?, additionalDirectories? }` on the parent
  before anything carrying the child's `sessionId` (updates, `session_message`,
  `session` content references, and permission/elicitation requests). The first
  update announces, later ones patch (omitted unchanged, `null` clears, value
  replaces whole). `capabilities` is replaced as a whole and absent means
  nothing permitted. Clearing `cwd`/`additionalDirectories` means inherit from
  the immediate parent again.
- **Parents and nesting**: outer `sessionId` is the immediate parent. Nesting is
  unlimited, one parent per child, no moves, no cycles. A child session
  identifies a *conversation*, reusable across delegations (or a new child per
  delegation). The Agent must not guess a parent, must hold activity until it
  knows both child and parent, and once a child is exposed MUST keep tagging
  its requests and updates with the child's ID.
- **State** **(Δ v1)**: a child sends ordinary `state_update` on its *own*
  `sessionId`, and the Agent SHOULD mirror the snapshot in the parent's
  `subagent_update.state`. Same four states as sessions. No separate subagent
  lifecycle: work starts and stops repeatedly and `idle` never ends the
  association. A failure is `idle` + `stopReason:"error"`. `unknown` MUST be
  reported when observation is lost (a Client must not keep showing an earlier
  `running`). Neither announcing nor clearing `state` means running, idle or
  cancelled. A parent's prompt response doesn't mean children stopped, and the
  parent can act on results with no prompt pending (cancelled by `session/cancel`
  on the parent, with no response confirming).
- **Messages between sessions**: `session_message { messageId, senderSessionId?,
  recipientSessionId?, senderMessageId?, recipientMessageId?, content? }` and
  chunks, scoped to the outer session's transcript. Two views (outgoing in the
  parent, incoming in the child) have independent IDs, linked only by the
  explicit `...MessageId` fields (never by equal IDs). Parent→child instructions
  SHOULD be an incoming `session_message` in the child, not a
  `user_message_chunk`. Without evidence of the sender, omit it rather than
  assume the parent. Clients must not synthesize one view from the other or
  forward content. A `session` tool-call content item is a display reference only.
- **Allowlist of what a Client may send for a child**: responses to the Agent's
  requests for it (always), `session/cancel` while `capabilities.cancel` is
  present, and `tool_call/cancel` / `terminal/cancel` for an item that
  advertises its own `cancel`. Nothing else: no `prompt`, `resume`, `close`,
  `delete`, `set_config_option`, and no methods added in later versions. The
  Agent ignores `session/cancel` for a child without `cancel` and SHOULD reject
  other requests with `-32602`.
- **Cancellation**: cancelling a session also cancels descendant work it waits
  on (whether other descendants stop is up to the Agent). The Agent confirms
  with idle + `cancelled` on the parent's `subagent_update` (a child has no
  prompt response). A request pending for a child stays attributed to that child
  and is not resolved by marking tool calls cancelled. Once the descendant
  reports idle+cancelled the Client handles its requests as if it cancelled it
  directly. `session/close` on a parent cancels all descendants.
- **Resume**: reconnect through the parent only. On `session/resume` the Client
  discards held child state and capabilities (nothing permitted until a current
  `subagent_update` says so). Ancestors are announced parent before child on
  every resume. The Agent must not invent failure or cancellation because
  history lacks a final update. Replayed child updates keep their original
  `sessionId`, parent and per-session order. A recorded `session_message` is
  replayed with whatever participants the Agent has. Replay never re-sends old
  Agent requests.
- **Context**: children's cwd, directories, config (`config_option_update` on the
  child, read-only for the Client), and usage are reported by the Agent. A
  child's root set is its own effective `[cwd, ...additionalDirectories]`, which
  can be outside the parent's. The Agent must not use a child to bypass a
  restriction on the parent. A Client's stricter policy applies to what it gets.
- **Usage and cost**: a session's `cost` includes its descendants. A child with
  no reported cost has unknown cost, not zero. Clients MUST NOT add a child's
  cost to an ancestor's. Context usage is per session.
- Connection loss: Clients MUST NOT keep presenting last `running` as confirmed
  and MUST NOT infer unfinished work succeeded, failed or was cancelled.

## Notices and compaction

No Client capability in v2, and Clients may preserve, ignore or generically
display them.

- **`notice`** `{ severity: info|warning|error (open), title (non-empty plain
  text), description?, _meta? }`: live and non-history, no ID, not replayed.
  Delivery and display are not guaranteed, so it must never carry anything
  required for correctness, authorization, user action, task completion or
  fatal errors. Clients own presentation and dismissal (local, no ack) and may
  coalesce. `error` severity does not fail a request or imply a stop reason.
- **`compaction_update { compactionId, status?, summary?, error?, _meta? }`** and
  `compaction_summary_chunk { compactionId, content }`: one persistent
  ID-addressed entity. `status` (open): `in_progress`, `completed`, `failed`,
  `cancelled`, and is now a patch field (first-seen ID starts `in_progress`).
  The Agent MUST report a terminal status. The summary is unencrypted,
  user-displayable (no hidden prompt framing, omit if disclosure would reveal
  non-user-visible context). A concrete `summary` replaces accumulated chunks,
  and `null` or `[]` clears it. Replay follows live semantics and does not reset
  omitted patch fields. It does not tell the Client to discard history and does
  not replace `usage_update`.

## Errors

| Code | Meaning |
|---|---|
| -32700 | parse error |
| -32600 | invalid request: bad envelope, anything but `initialize` before init, repeated `initialize` after another request, a JSON array, `id: null` with a method |
| -32601 | method not found |
| -32602 | invalid params |
| -32603 | internal error |
| -32800 | request cancelled |
| -32000 | authentication required (or failed `auth/login`) |
| -32002 | resource not found (always with `data.sessionId` for sessions) |
| -32022 | unsupported protocol version (`data: {requested, supported}`) |

Unknown codes are a generic failure. Custom errors avoid `-32768..-32000` and
`-32899..-32800`. Several v1 SHOULDs are MUSTs **(Δ v1)**:

- Unimplemented methods, or capability-gated methods not advertised: `-32601`
  (notifications: ignore unknown ones). Exception: `tool_call/cancel` /
  `terminal/cancel` get `-32602` for non-cancellable items.
- Session-scoped request for a non-existent or inactive session: `-32002` +
  `data.sessionId` (`session/prompt`, `set_config_option`, `tool_call/cancel`,
  `terminal/cancel`, `session/resume` of a missing or deleted session).
  `close` and `delete` of a missing session SHOULD succeed (Clients treat
  `-32002` as success). A child is not an unknown session but other requests
  naming one SHOULD get `-32602`.
- Bad cursor, bad `systemPrompt`, bad MCP config, bad `replayFrom`, bad
  `idempotencyKey`, unadvertised content type: `-32602`.
- Pre-init request, or repeated `initialize` after another request: `-32600`.
- Cancellation: receivers MUST ignore `$/cancel_request` for an unknown or
  already-answered ID and MUST NOT send a second response. A pre-insertion
  `session/prompt` can be answered `-32800`, an inserted one cannot.

## Security model (summary)

Unchanged in spirit from v1. The Agent runs with its process's permissions and
ACP does not sandbox. Treat every Agent-authored string, URI and path as
untrusted (including replayed content). Permission requests are advisory.
Intermediaries can read and alter everything and must not answer permissions or
elicitations for the user outside user-set policy. Multi-user Agents scope
list/resume/permissions/elicitation to the owning user. Credentials never go in
`_meta`. IDs are not credentials. With the Client file/terminal APIs removed,
the main Client-side attack surface is now the MCP servers it supplies and the
Agent-owned terminals it renders.

## Supporting v1 and v2 side by side

Recommended: negotiate once per connection, then use only that version's
handlers.

- Keep v1 and v2 schemas, generated models and fixtures **fully separate**.
  Exposing unstable-v2 types does not advertise support.
- A dual-stack Agent: a v1 request selects v1, a v2 request selects v2, a newer
  unsupported version selects v2. A v1-only Agent answers a v2 request with v1,
  and the Client continues only if it supports v1 (a v1 Agent reads a v2 request
  *without* `clientCapabilities`/`clientInfo`, so fs, terminal and terminal-auth
  support is lost, and a Client that needs them reconnects with a v1
  `initialize`). A v2-only Agent rejects a v1 request with `-32022`
  `{requested:1, supported:[2]}` without running any v2 session work or
  requiring v2 Client identity.
- Share business logic, tool execution and persistence behind **per-version
  adapters**. Do not blanket-convert structs: the lifecycle differs (v1 holds
  the prompt open, v2 acknowledges insertion and reports completion as
  `state_update`) and v1 projections cannot represent every v2 update.
  Cross-version session access must be defined explicitly.
- Advertise only what each adapter implements.
- Validate a dual-stack endpoint with real v1 and v2 Clients: correct routing and
  capabilities, fallback from v2 Client to v1 Agent, a higher requested version
  (including unparseable params), full prompt and cancel flows asserting each
  version's *completion signal*, and v1 still working with v2 disabled.

## Migration cheatsheet (v1 → v2)

| v1 | v2 |
|---|---|
| `clientCapabilities`/`agentCapabilities`, `clientInfo`/`agentInfo` (optional) | `capabilities`, `info` (required) |
| boolean markers | object markers (`{}`) |
| `promptCapabilities`, `mcpCapabilities` | `session.prompt`, `session.mcp` |
| `loadSession`, `session/load` | `session/resume` + `replayFrom:{type:"start"}` + `replay_complete` |
| `authenticate`, `logout`, `auth.logout` | `auth/login`, `auth/logout` (no marker) |
| auth method `id` | `methodId` + required `type`, terminal `env` array |
| prompt response `{stopReason}` | prompt response `{messageId}`, idle `state_update {stopReason}` |
| error response to prompt for failures mid-turn | idle + `stopReason:"error"` + `error` |
| `tool_call` + `tool_call_update` | `tool_call_update` only (+ `tool_call_content_chunk`) |
| `plan` | `plan_update {type:"items", planId}` |
| diff `{path,oldText,newText}` | diff `{changes, patch}` |
| `fs/*`, client `terminal/*` | removed, use a Client-supplied MCP server. Agent-owned `terminal_update` for display |
| `session/set_mode`, `modes`, `current_mode_update` | config options with `category:"mode"` |
| option `id`/`group` | `configId`/`groupId`, required `type` on set |
| permission `toolCall` | `title`, `description?`, `subject?` |
| client answers every pending permission on cancel | Agent cancels the relevant ones with `$/cancel_request` |
| SSE MCP, untyped stdio config | `type` required, SSE removed |
| optional `messageId` on chunks | required, plus `user_message`/`agent_message`/`agent_thought` upserts |
| client capabilities for plan, notices, compaction, subagents, boolean options | none, always on |
| command `{name,description,input:{hint}}` | `{type:"command", ..., input:{type:"text", hint}}` |
| string `pattern` in elicitation forms | removed |
