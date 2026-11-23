# Agent Client Protocol v1

A design reference for ACP protocol version 1, condensed from the upstream
protocol documentation (the published prose pages and the JSON Schema). Where
this file and the schema disagree, the schema wins. ACP v2 is covered in
[acp-v2.md](acp-v2.md); v1 and v2 differ in the prompt lifecycle, so read both
before choosing one.

Key words (MUST, SHOULD, MAY) carry their RFC 2119 meaning, as upstream uses
them.

## What it is

ACP lets a **Client** (an editor or other UI) drive an **Agent** (a program that
uses a model to modify code, usually a subprocess of the Client). It is
JSON-RPC 2.0 over a bidirectional byte stream. Both sides expose methods and
send notifications. The Agent calls back into the Client for permissions,
file access, terminals and user questions.

Schema: JSON Schema draft 2020-12, titled "Agent Client Protocol". The top level
is an `anyOf` of message types and types live in `$defs`. A stable schema and an
`unstable` schema (draft features) are published for each version.

## Conventions

- Property keys are `camelCase`. Values of discriminator fields are `snake_case`.
- JSON-RPC envelope fields follow JSON-RPC.
- All file paths MUST be absolute. Line numbers are 1-based.
- Unknown capabilities are unsupported. New capabilities are not breaking changes.
- Omitted and `null` are equivalent for most optional fields. Patch-style
  updates (below) are the exception, and each says so.
- `_meta` (`{ [key]: unknown }`) is allowed on most types. Custom methods start
  with `_`. Custom capabilities go in a capability object's `_meta` under a
  reverse-DNS key (`com.example/feature`). Implementations MUST NOT add custom
  fields at the root of a spec type. `traceparent`, `tracestate` and `baggage`
  at the root of `_meta` are reserved for W3C trace context.

## Transport

Only stdio is standardized.

- Client launches the Agent as a subprocess.
- One JSON-RPC message per line, UTF-8, newline-delimited, no embedded newlines.
- Agent: JSON-RPC on stdout, logs on stderr (Client may capture or ignore).
  Nothing else may appear on stdout. Client may write only ACP messages to stdin.
- Closing stdin and terminating the process ends the connection.
- Streamable HTTP/WebSocket is being specified separately and is not part of v1.
  Custom transports MUST preserve JSON-RPC framing and lifecycle.

## Lifecycle at a glance

```
Client                                   Agent
  initialize  ───────────────────────────▶   negotiate version + capabilities
  (authenticate / terminal login)            only if authMethods demands it
  session/new  | session/load | session/resume
  session/prompt ───────────────────────▶   request stays pending for the turn
        ◀─────────── session/update*         streamed content, tool calls, plans…
        ◀─────────── session/request_permission, fs/*, terminal/*, elicitation/create
  session/cancel ───────────────────────▶   (notification)
        ◀─────────── session/prompt response {stopReason}
```

The defining v1 property: **the `session/prompt` response is the end of the
turn**. It carries the stop reason, and a failure during the turn comes back as
an error response to it.

## Roles and methods

Agent methods (Client calls these):

| Method | Required | Purpose |
|---|---|---|
| `initialize` | yes | version and capability negotiation |
| `authenticate` | if auth needed | protocol-driven login |
| `session/new` | yes | create a session |
| `session/prompt` | yes | send a user message, runs a turn |
| `session/load` | `loadSession` | resume with full history replay |
| `session/resume` | `sessionCapabilities.resume` | resume without replay |
| `session/close` | `sessionCapabilities.close` | cancel work and free the session |
| `session/list` | `sessionCapabilities.list` | enumerate sessions |
| `session/delete` | `sessionCapabilities.delete` | remove from the list |
| `session/set_mode` | deprecated | switch operating mode |
| `session/set_config_option` | when `configOptions` offered | change an option |
| `logout` | `auth.logout` | end authenticated state |

Agent notifications: `session/cancel`.

Client methods (Agent calls these):

| Method | Required | Purpose |
|---|---|---|
| `session/request_permission` | yes | ask the user to approve an operation |
| `fs/read_text_file`, `fs/write_text_file` | `fs.*` | file access incl. unsaved editor buffers |
| `terminal/create`, `output`, `wait_for_exit`, `kill`, `release` | `terminal` | run and manage commands |
| `elicitation/create` | `elicitation.form` / `.url` | structured questions to the user |

Client notifications: `session/update` (the main Agent→Client stream) and
`elicitation/complete`.

Either side: `$/cancel_request`.

## Initialization

Client sends `initialize` first and MUST NOT send other requests until it
completes. Agents SHOULD reject other requests before then with `-32600`.

```json
{ "method": "initialize", "params": {
  "protocolVersion": 1,
  "clientCapabilities": { "auth": {"terminal": true},
    "fs": {"readTextFile": true, "writeTextFile": true}, "terminal": true },
  "clientInfo": { "name": "my-client", "title": "My Client", "version": "1.0.0" } } }
```

Response carries `protocolVersion`, `agentCapabilities`, `agentInfo` and
`authMethods`. `clientInfo`/`agentInfo` are self-reported: do not use them for
security decisions.

**Version negotiation.** `protocolVersion` is a single integer, bumped only for
breaking changes. The Client sends the latest it supports. If the Agent supports
it, it replies with the same number, otherwise with the latest it supports. If
the Client cannot speak the returned version it SHOULD disconnect.

**Capabilities.** All optional. Omitted means unsupported.

Client capabilities: `auth.terminal` (bool), `fs.readTextFile`, `fs.writeTextFile`,
`terminal`, `elicitation` (`{form?, url?}`; `{}` advertises nothing, unlike MCP),
`plan` (`{}` enables `plan_update`/`plan_removed`), `session.configOptions.boolean`,
`session.compaction`, `session.notices`, `subagents`.

Agent capabilities: `loadSession`, `promptCapabilities` (`image`, `audio`,
`embeddedContext`), `mcpCapabilities` (`http`, `sse`), `auth.logout`,
`sessionCapabilities` (`list`, `resume`, `close`, `delete`,
`additionalDirectories`, `systemPrompt` with optional `mode.override`).

Baseline every Agent MUST support: `session/new`, `session/prompt`,
`session/cancel`, `session/update`, `text` and `resource_link` prompt content,
and MCP over stdio.

The rule that gates everything: **a side MUST NOT send a feature the peer has
not advertised.** Agents must not send `notice`, `compaction_*`, `plan_update`,
`subagent_update` etc. without the matching Client capability.

## Authentication

`authMethods` in the `initialize` response lists options. Each has `id`, `name`,
optional `description`, and `type` (default `agent`).

- `agent`: Client calls `authenticate { methodId }`, Agent returns `{}`.
- `terminal`: Client launches the *same configured Agent program* interactively
  with the method's `args` appended and `env` applied, waits for exit (status 0
  is success), then reconnects and re-initializes. The Client MUST NOT send
  `authenticate` for it. The descriptor cannot supply a command. Requires
  `clientCapabilities.auth.terminal`. Launch safety: pass args without a shell,
  do not apply `env` that changes program lookup, loading or trust (`PATH`,
  `LD_*`, `DYLD_*`, `NODE_OPTIONS`, `PYTHONPATH`, proxies, TLS roots), show args
  and env before launch.
- `logout` ends the authenticated state (needs `auth.logout`). What happens to
  running sessions is undefined: they may be terminated or return
  `auth_required` (-32000) later.

Authentication-gated requests fail with `-32000` until the user authenticates.

## Sessions

A session is one conversation with its own context, history and state.

### Creating

`session/new { cwd, mcpServers, additionalDirectories?, systemPrompt? }` returns
`{ sessionId, modes?, configOptions? }`. The Agent allocates a unique id.

- `cwd`: absolute, the primary root and base for relative paths, used regardless
  of where the Agent process started. The effective root set is
  `[cwd, ...additionalDirectories]`. It declares scope. It confines shell
  commands only if a sandbox enforces it. Implementations that enforce it
  should check the resolved path at time of use, compare by path component,
  and fail closed.
- `additionalDirectories`: needs `sessionCapabilities.additionalDirectories`.
  On `load`/`resume` the Client MUST resend the full intended list. Omission
  does not restore stored roots.
- `systemPrompt { mode: "append" | "override", content: [text blocks] }`: needs
  `sessionCapabilities.systemPrompt`. `append` adds to the Agent's own
  instructions at system-level precedence. `override` replaces the built-in
  instructions (not tools, MCP servers or permissions) and needs
  `mode.override`. Only text blocks, no empty `content`. Unsupported mode,
  block type or oversize content: Agent MUST return an error (`-32602`
  recommended) and not create the session. It carries system-level weight, so
  Clients should send only trusted content.
- `mcpServers`: see below.

### Loading, resuming, closing

- `session/load { sessionId, cwd, mcpServers }`: Agent MUST replay the entire
  conversation as `session/update` notifications, then respond `{}`. Requires
  `loadSession`.
- `session/resume`: same params, MUST NOT replay, responds when ready. Requires
  `sessionCapabilities.resume`.
- `session/close { sessionId }`: cancels ongoing work as if `session/cancel`
  were sent and frees resources. Idempotent. Treat `-32002` as success.
- Replay of compactions follows live semantics and requires the Client's
  `session.compaction`. Live-only notices SHOULD NOT be replayed.

### Listing and metadata

`session/list { cwd?, cursor? }` returns `{ sessions: SessionInfo[], nextCursor? }`
with opaque cursors (missing `nextCursor` means end; invalid cursor is
`-32602`). `SessionInfo`: `sessionId`, `cwd`, `additionalDirectories?`, `title?`,
`updatedAt?` (ISO 8601), `_meta?`. It does not restore anything. Use
`session/load` afterwards. Child (subagent) sessions are not listed.

`session_info_update` (via `session/update`) patches `title`, `updatedAt`,
`_meta`: omitted leaves unchanged, `null` clears. `session/delete { sessionId }`
removes from future lists. Deleting something already gone succeeds, and
`-32002` is treated as success. Behavior of load or delete on an active deleted
session is implementation-defined.

### MCP servers

Clients pass MCP servers the Agent should connect to. Agents SHOULD connect to
all of them, which lets a Client inject its own tools.

- stdio (mandatory, no `type`): `{ name, command (absolute), args, env[{name,value}] }`
- http (`mcpCapabilities.http`): `{ type:"http", name, url, headers[{name,value}] }`
- sse (`mcpCapabilities.sse`, deprecated upstream): same shape, `type:"sse"`

`env` and `headers` values are credentials. Launch `command` with `args` as
separate arguments without a shell. Do not put these values in updates, errors,
`_meta`, listings, history, model context or logs. Send `headers` only to the
configured origin. Clients should not send a stdio server taken from workspace
files or Agent output without user approval of the exact command, args and env
names.

## The prompt turn

### Request

`session/prompt { sessionId, prompt: ContentBlock[] }`. Content must respect the
Agent's prompt capabilities. The Agent runs the model, which may emit text and
tool calls, and the turn continues until the model stops without asking for
more tools or the turn is stopped.

### Reporting output: `session/update`

`params: { sessionId, update }` and `update.sessionUpdate` is the discriminator.

| `sessionUpdate` | Meaning | Gate |
|---|---|---|
| `user_message_chunk`, `agent_message_chunk`, `agent_thought_chunk` | streamed content, optional opaque `messageId` groups chunks | |
| `tool_call`, `tool_call_update` | tool lifecycle | |
| `plan` | single checklist, replace whole | |
| `plan_update`, `plan_removed` | identified plans | Client `plan` |
| `available_commands_update` | slash commands | |
| `current_mode_update` | mode changed | deprecated |
| `config_option_update` | complete option list | |
| `session_info_update` | title, updatedAt, `_meta` patch | |
| `usage_update` | `used`, `size`, optional `cost{amount,currency}` | |
| `notice` | live user-facing notice | Client `session.notices` |
| `compaction_update`, `compaction_summary_chunk` | context compaction entity | Client `session.compaction` |
| `subagent_update`, `session_message`, `session_message_chunk` | child sessions | Client `subagents` |

Updates are not limited to active turns. Agents may advertise commands right
after creating a session and replay history on load.

`usage_update`: `cost` of a session includes its descendants' costs. A child
with no reported cost has unknown cost, not zero. Clients must not add a
child's cost to an ancestor's. Context usage is per session.

### Stop reasons

`end_turn`, `max_tokens`, `max_turn_requests`, `refusal`, `cancelled`. The Agent
MUST answer the original `session/prompt` with one. `refusal` also implies the
refused prompt and everything after it is excluded from the next prompt, and the
UI should show that.

### Cancellation of a turn

Client sends `session/cancel { sessionId }`. Then:

- Client SHOULD immediately mark unfinished tool calls of the turn `cancelled`
  (a local display hint).
- Client MUST answer all pending `session/request_permission` requests with the
  `cancelled` outcome.
- Agent SHOULD stop model requests and tool calls ASAP, MAY still send trailing
  `session/update`s, and MUST then respond to the prompt with stop reason
  `cancelled`. Agents MUST catch abort exceptions from their model libraries and
  return `cancelled`, not an error.
- Client SHOULD still accept tool-call updates that arrive after sending cancel.

### Request cancellation (`$/cancel_request`)

A generic, optional mechanism for any request. The receiver MAY abort and MUST
still answer once: either a normal response (for example prompt with
`cancelled`) or error `-32800`. Internal cancellation (user closes the IDE,
context limit, timeout) SHOULD produce the same `-32800`. Propagating cancel to
nested requests (for example an outstanding `terminal/create`) is optional.

## Tool calls

The Agent runs tools itself and reports them. It may use Client capabilities
(permissions, `fs`, `terminal`).

`tool_call` (create) fields: `toolCallId` (unique in session), `title` (human
text), `name?` (programmatic, informational only, not a capability or
authorization), `kind`, `status` (default `pending`), `content[]`, `locations[]`,
`rawInput`, `rawOutput`.

- `kind`: `read`, `edit`, `delete`, `move`, `search`, `execute`, `think`,
  `fetch`, `switch_mode`, `other` (default). Presentation hint only.
- `status`: `pending` (input streaming or awaiting approval), `in_progress`,
  `completed`, `failed`.
- `tool_call_update`: only `toolCallId` required. Present fields change.
  `content` replaces the whole array. For `rawInput`/`rawOutput`, omitted and
  `null` both mean unchanged. `name` can be set or replaced but not cleared.
- Content items: `content` (wrapping a content block), `diff { path, oldText?,
  newText }` (`oldText` null for a new file), `terminal { terminalId }` (live
  output, kept visible after release), `session { sessionId }` (display-only
  link to another session, for example a subagent).
- `locations[] { path, line? }`: lets the Client "follow the agent". Display
  only: do not auto-open outside the root set.

### Permission requests

`session/request_permission { sessionId, toolCall: ToolCallUpdate, options[] }`.
Each option: `optionId`, `name`, `kind` in `allow_once | allow_always |
reject_once | reject_always`. Response `outcome` is `{outcome:"cancelled"}` or
`{outcome:"selected", optionId}`. Clients may auto-answer by user policy.

- The **Agent owns remembered decisions**. `allow_always`/`reject_always`
  mean the Agent stores the decision, picks its scope and lifetime, and stops
  asking. `kind` is just a UI hint, and the option `name` should state the
  scope.
- Permission is advisory: the Agent performs the operation and is trusted to
  honor the answer. Agents should not treat unknown outcomes as approval,
  should do only what was approved and re-ask if the command or resolved
  target changes, and should not name an option contrary to its `kind`.
- Clients should describe the operation from `kind`/`locations`/content,
  present `title` as Agent-authored, show commands and paths in full, and not
  auto-approve on `title` or option `name` alone.

## Client-provided file system

Needs `fs.readTextFile` / `fs.writeTextFile`. Gives the Agent unsaved editor
state and lets the Client observe edits.

- `fs/read_text_file { sessionId, path, line?, limit? }` returns `{ content }`.
- `fs/write_text_file { sessionId, path, content }` returns `{}`. The Client
  MUST create the file if missing.
- The Client does the operation itself, so it does its own checks: path against
  the session root set at time of use, and ask the user before writes to files
  that control execution or approval (VCS hooks, editor or Agent settings, MCP
  config). A permission request from the Agent does not replace this.
- Missing file: `-32002`.

## Client-provided terminals

Needs `terminal`.

- `terminal/create { sessionId, command, args?, env?, cwd?, outputByteLimit? }`
  returns `{ terminalId }` immediately. The command runs in the background.
  Output beyond the byte limit is truncated from the start at a character
  boundary.
- `terminal/output` returns `{ output, truncated, exitStatus? }` without
  waiting. `terminal/wait_for_exit` returns `{ exitCode?, signal? }`.
  `terminal/kill` stops the command but keeps the terminal valid.
  `terminal/release` kills if still running and invalidates the ID. The Agent
  MUST release every terminal it creates.
- Embed in a tool call with `{ type:"terminal", terminalId }`.
- Timeouts are built from these primitives: create, race a timer against
  `wait_for_exit`, then `kill`, `output`, `release`.
- Client security: require `cwd` inside the root set, apply policy to the full
  command, args **and env**, neutralize terminal control sequences in output
  (clipboard writes, query responses, links).

## Content blocks

A superset of MCP 2025-06-18 `ContentBlock` (ACP adds optional `uri` on images),
so MCP tool output can be forwarded unchanged.

| `type` | Fields | Prompt requirement |
|---|---|---|
| `text` | `text`, `annotations?` | always supported, may be Markdown |
| `resource_link` | `uri`, `name`, `mimeType?`, `title?`, `description?`, `size?` | always supported |
| `image` | `data` (base64), `mimeType`, `uri?` | `image` capability |
| `audio` | `data`, `mimeType` | `audio` capability |
| `resource` | `resource` = text (`uri`,`text`) or blob (`uri`,`blob`), `mimeType?` | `embeddedContext`, preferred for @-mentions |

Agent content is untrusted. Render Markdown with raw HTML disabled or
sanitized, do not fetch Agent-supplied remote URIs without user action (fetch
only `https:`/`data:` without credentials, refuse loopback and private
networks, bound size), no scripts from SVG, do not read `file:` URIs outside
the root set, and show the full target when a link differs from its text.

## Plans

`plan { entries[{content, priority: high|medium|low, status:
pending|in_progress|completed}] }`. Every update carries the **complete** list
and the Client replaces the checklist. An empty list empties it. With the Client
`plan` capability the Agent SHOULD use `plan_update { plan }` instead, where
`plan` is `{type:"items", planId, entries}` or `{type:"markdown", planId,
content, path?}`. Each update replaces the plan with that `planId` wholesale,
even across types. `plan_removed { planId }` is for cancelled plans (finished
plans need not be removed). Clients MUST ignore `plan_removed` for an unknown id.

## Slash commands

`available_commands_update { availableCommands[{name, description, input?{hint}}] }`
can be sent any time and replaces the list. Running one is just a normal prompt
whose text starts with `/name ...`. The Agent recognizes the prefix. Other
content may accompany it. Keep Agent commands visually distinct from the
Client's own.

## Session modes (deprecated) and config options

**Modes**: `modes { currentModeId, availableModes[{id,name,description?}] }` in
session responses, `session/set_mode { sessionId, modeId }`, and
`current_mode_update`. Superseded by config options. During transition, Agents
SHOULD send both and keep them in sync. Clients with config-option support use
only `configOptions`.

**Config options** are Agent-defined selectors (model, thought level, mode...).
`configOptions[]` appears in session responses and `config_option_update`:
`{ id, name, description?, category?, type, currentValue, options? }`.

- `type: "select"` with `options` as a flat list of `{value,name,description?}`
  or a list of groups `{group,name,options}` (not mixed). `type:"boolean"` only
  if the Client advertised `session.configOptions.boolean`, else Agents omit it
  or offer a select.
- Array order is Agent priority. Clients SHOULD respect it, including to break
  ties within a category.
- `category`: `mode`, `model`, `model_config`, `thought_level`. `_`-prefixed are
  custom, others reserved. UX hint only, never required for correctness.
- Every `currentValue` must be workable even if the Client ignores or cannot
  render the option. Clients ignore unknown `type`s.
- `session/set_config_option { sessionId, configId, value }` (plus
  `type:"boolean"` for booleans) works anytime, even mid-turn. The response and
  every `config_option_update` carry the **complete** list, since changing one
  option can alter others.

## Elicitation

Structured questions to the user, adapted from MCP elicitation. Needs
`clientCapabilities.elicitation.form` and/or `.url`.

`elicitation/create` params: `mode` (required, no default), `message`, scope
(`sessionId` + optional `toolCallId`, or `requestId` for out-of-session), and:

- **form**: `requestedSchema`, a flat object schema of primitive and enum
  properties. Clients SHOULD pre-fill defaults and validate, Agents SHOULD
  re-validate. MUST NOT be used for secrets (passwords, API keys, tokens,
  private keys, payment credentials). If URL mode is unavailable, the Agent MUST
  NOT fall back to a form.
- **url**: `elicitationId`, `url`. For sensitive or external flows such as OAuth.
  `accept` means consent to open the URL, not completion. Credentials obtained
  this way never come back over ACP or into model context. Agent MAY later send
  `elicitation/complete { elicitationId }` to the same Client. Clients ignore
  unknown ids. The Agent must verify the user who completes the flow is the one
  who started it. Clients MUST show the full URL, get consent, not prefetch it,
  and open it somewhere the model cannot inspect.

Response: `action` is `accept` (optional `content`), `decline` or `cancel`.
Agents must handle all three and failures. Requesting an unadvertised mode is
`-32602`. Clients must show who is asking, give decline and cancel controls, and
let the user review form responses.

## Subagents

An Agent can expose delegated work as **child sessions**: its own `sessionId`,
owned by the Agent, observed (not driven) by the Client. Optional for Agents.
Gated by Client `subagents: {}`. There is no Agent capability. The first
`subagent_update` is the signal.

- **Announce** with `subagent_update` on the parent: `{ sessionId (child), title?,
  description?, capabilities?{cancel?}, state?, cwd?, additionalDirectories? }`.
  The first update announces and later ones patch. Every field except
  `sessionId` is a patch (omitted unchanged, `null` clears, value replaces
  whole). Clearing `cwd`/`additionalDirectories` makes the child inherit from
  its parent again. The Agent MUST announce a child before any update or request
  carrying its `sessionId`, including after every `load`/`resume`, parents
  before children. It must not guess a parent.
- A child's updates and requests (permissions, fs, terminals, elicitation) use
  the child's `sessionId`, with the same capability checks as the parent. The
  Agent must not use a child to bypass a restriction on the parent. IDs such as
  `toolCallId` are scoped per session.
- **Nesting** is arbitrary: the outer `sessionId` is the immediate parent. One
  parent per child and no cycles. A `sessionId` is a conversation, not a task:
  it can be reused for more work, or a new child announced for each delegation.
- **Messages between sessions**: `session_message` / `session_message_chunk`
  carry `messageId`, `senderSessionId`, `recipientSessionId`, and may link two
  views with `senderMessageId`/`recipientMessageId`. Parent→child instructions
  SHOULD be an incoming `session_message` in the child (not a
  `user_message_chunk`). Absent evidence of the sender, omit it rather than
  assume the parent. Clients must not synthesize one view from the other or link
  by equal ids. Content fields follow patch rules (`content` replaces,
  chunks append).
- **Work state** lives only in the parent's `subagent_update.state`:
  `running`, `requires_action`, `idle` (+ optional `stopReason`, which may be
  `error` with an `error` object), `unknown`. There is no lifecycle beyond
  that. Work can start and stop repeatedly. The Agent must report
  `unknown` when it can no longer observe a child. Clients must not present an
  earlier `running` as confirmed then. The parent's prompt response does not
  mean children stopped, and a parent can keep acting on results with no
  prompt pending.
- **Restrictions (allowlist)**: the Client may send for a child only responses to
  the Agent's requests, and `session/cancel` while `capabilities.cancel` is
  present. Never `prompt`, `load`, `resume`, `close`, `delete`, `set_mode`,
  `set_config_option`. Cancelling a session also cancels the descendants it is
  waiting on. `session/close` on a parent cancels all descendants.
- **Reconnect**: only through the parent. On load/resume the Client discards
  child state and capabilities. Replayed child state is historical (before the
  `session/load` response). The Agent MUST NOT invent a failure or cancellation
  just because history lacks a final update.
- Child config is reported with `config_option_update` on the child and is
  read-only for the Client. Directories are reported on `subagent_update`, not
  negotiated.

## Notices and compaction (gated)

- **`notice`** `{ severity: info|warning|error (open enum), title, description?,
  _meta? }`: a live, non-history message. No id and no lifecycle. May be dropped,
  so it must never carry anything needed for correctness, authorization,
  required user action or fatal errors. Not replayed. Without the capability,
  the Agent may fall back to an ordinary agent message.
- **`compaction_update`** `{ compactionId, status: in_progress|completed|failed|
  cancelled, summary?, error?, _meta? }` and **`compaction_summary_chunk`**
  `{ compactionId, content }`. One persistent, ID-addressed timeline entity. The
  first notification fixes its timeline position. Fields other than
  `compactionId`/`status` are patches. A concrete `summary` replaces
  accumulated chunks and later chunks append. The Agent must report a terminal
  status. The summary is user-displayable, never hidden prompt framing. It does
  not tell the Client to discard history and does not replace `usage_update`.

## Errors

Standard JSON-RPC error objects `{code, message, data?}`.

| Code | Meaning |
|---|---|
| -32700 / -32600 / -32601 / -32602 / -32603 | JSON-RPC standard (parse, invalid request, method not found, invalid params, internal) |
| -32800 | request cancelled |
| -32000 | authentication required |
| -32002 | resource not found (session, file, terminal) |

Unknown codes are a generic failure. Custom errors should avoid `-32768..-32000`
and `-32899..-32800`.

Common cases: unimplemented or ungated-but-unadvertised method is `-32601`
(SHOULD in v1). Bad or unsupported params (invalid cursor, unknown `configId` or
mode, bad `systemPrompt`) is `-32602`. A session-scoped request for an unknown
or inactive session SHOULD be `-32002` with `data.sessionId`. Close and delete of
a missing session still succeed. A child session is not unknown, and other
requests naming a child get `-32602`.

## Security model (summary)

- The Agent runs with whatever permissions its process has. ACP does not sandbox.
- Treat every Agent-authored string, URI and path as untrusted, including
  replayed content. Files, web pages, tool results and MCP servers can steer the
  model (prompt injection).
- Permission requests are advisory. The Client performs `fs`/`terminal` itself
  and must check them.
- Do not put credentials in `_meta` (including trace keys). Session, message,
  tool-call and terminal ids are not credentials.
- An intermediary (proxy) can read and alter everything and each side treats it
  as the peer. Multi-user Agents scope list, load, permissions and elicitations
  to the owning user.
- Out of scope: a malicious Agent binary, detecting prompt injection, OS
  sandboxing.

## Design observations

- Capabilities are the only extension gate. Anything new is additive and
  capability-gated, so old peers see nothing new.
- The Agent drives the Client (permissions, fs, terminals, elicitation) as
  requests inside an outstanding prompt request, so both sides must be able to
  send and answer concurrently.
- Patch semantics (omit = keep, `null` = clear, value = replace) are applied
  per feature rather than globally in v1. v2 makes them uniform.
- Where v1 ties turn end to the prompt response, it entangles acceptance,
  completion, replay and background work. This is what v2 redesigns.
