# Turn

One line typed into `akern-cli`. The model makes two `shell` calls together,
`cat README.md` and `ls src`, then answers. Nothing interrupts it.

`A → B: …` is data moving from one place to another. Every line `akern` says
goes onto its bus and from there, in the same order, to its stdout (piped to
`render.jq`), to the session file and to its own actors (loop, input, tools, model, inbox);
`akern → bus: …` below means all of those. Lines are in [akern.md](akern.md), the
request and the provider's stream in [providers.md](providers.md), the actors in
[design.md](design.md#one-stream-five-actors).

- `akern-cli` starts, picks the model (`AKERN_MODEL`, `$AKERN_HOME/model`, or
  `akern-model`) and starts `akern | render.jq`.
- `akern` launches the `system-prompt` program (`$AKERN_HOME/system-prompt`, else
  `$AKERN_HOME/system/system-prompt`).
- `prompt → akern`: your `system.md`, the skill list and the `AGENTS.md` files.
  `system-prompt` exits; this text is the startup system prompt.
- `akern` creates the session file. `akern → bus`: the `session` header, a
  `settings` entry with the model and that text (what every request uses until
  another one is appended), and `hello`. `render.jq` shows
  `› waiting for input`.
- You type `what is in this project?`.
- `terminal → akern`: the line, on `akern`'s stdin.
- `akern → bus`: `queued`; the line waits in the inbox.
- The inbox delivers it. `akern → bus`: `run_start` (`render.jq` shows
  `… working`), then the line as a user `message`.
- The inbox tells the model to continue; it starts turn 1. `akern` launches the
  provider (`$AKERN_HOME/providers/NAME`, else `$AKERN_HOME/system/providers/NAME`).
- `akern → provider`: the request on its stdin as one line (model, system
  prompt, the messages in the session, tool schemas, params).
- `akern → bus`: `turn_start`, turn 1, with the id its reply will get.
- `provider → akern → bus`: `text_delta`, `tool_use_start` and
  `tool_use_delta` lines, passed through unchanged. `render.jq` prints the
  text; the session file holds them back.
- `provider → akern`: a `result` line, the whole assistant message with
  `stop: tool_use`. `akern` keeps it rather than passing it on. The provider
  exits.
- `akern → bus`: the reply as a `message`, byte for byte, then `turn_end`. The
  session file drops the held `turn_start` and deltas: the message seals them.
- The tools actor sees a reply that asks for tools, and only now starts the
  calls. `akern → bus`: a `tool_start` for each, in order, as its program
  starts, with its pid.
- `akern` runs `bash -c 'cat README.md'` and `bash -c 'ls src'` at once, in
  its working directory; bash runs each program in its own place.
- `cat → akern`, `ls → akern`: their stdout and stderr. `akern → bus`:
  `tool_progress` for each chunk. Each exits.
- `akern → bus`: a `block` for each, as it finishes: its `tool_result`, exactly
  as the results message will hold it; the inbox puts it in the call's slot.
- Both slots are filled. `akern → bus`: both results in one user `message`, in
  call order (`cat`, then `ls`). The session file drops the `block`s it held
  for them (and the calls' own lines, where nothing came between).
- The inbox tells the model to continue; it starts turn 2. `akern` launches a
  new provider. `akern → provider`: the request, now three messages, as one
  line. `akern → bus`: `turn_start`, turn 2.
- `provider → akern → bus`: `text_delta` lines. `render.jq` prints the answer.
- `provider → akern`: a `result` line with `stop: end_turn`. The provider
  exits. `akern → bus`: the reply as a `message`, then `turn_end`.
- Nothing more is due. `akern → bus`: `run_end`, `done`, then `settled`.
  `render.jq` shows `› waiting for input`.
- `akern` waits for the next line. At EOF (Ctrl-D) it exits, then `render.jq`
  and `akern-cli`.

The model runs only in a provider process: a new one per turn, here, since
this provider's results do not stay; one whose results stay (`claude-code`)
gets turn 2's request on the same stdin instead of exiting
([providers.md §1](providers.md#1-finding-and-running-a-provider)). A line
arriving while idle starts a run, and the message that answers a turn starts
the next one; results that finish together share one message. A
`"wait": false` call would get no answer in the results message: a `pending`
line would name it (and answer the turn by itself when no call had ended), the
provider would answer it with a placeholder, and its later exit would wake the
model again with its late `tool_result`
([akern.md: delivery modes](akern.md#delivery-modes)). A line sent `now` while
a reply streams goes into the conversation at once: the inbox puts `preempt`,
the model actor stops the provider, the partial reply is sealed after that line, and in the context it sits
before it, where it began. Once no run, queued item or job is left, `akern`
sends `settled`.
