# Turn

One line typed into `akern-cli`. The model calls `read` (`README.md`) and `exec`
(`ls src`) together, then answers. Nothing interrupts it.

`A → B: …` is data moving from one place to another. `akern`'s stdout is piped
to `render.jq`, so every event goes `akern → render.jq`; events are in
[akern.md](akern.md), the request and the provider's stream in
[providers.md](providers.md).

- `akern-cli` starts, picks the model (`AKERN_MODEL`, `$AKERN_HOME/model`, or
  `akern-model`) and starts `akern | render.jq`.
- `akern` launches the `prompt` program (`$AKERN_HOME/prompt`, else
  `$AKERN_HOME/system/prompt`).
- `prompt → akern`: your `system.md`, the skill list and the `AGENTS.md` files.
  `prompt` exits; this text is the system prompt of every request this `akern`
  sends.
- `akern` creates the session file.
- `akern → render.jq`: a `hello` event, naming the session and the model.
  `render.jq` shows `› waiting for input`.
- You type `what is in this project?`.
- `terminal → akern`: the line, on `akern`'s stdin.
- `akern → render.jq`: a `queued` event; the line waits in the inbox.
- `akern → render.jq`: a `run_start` event. `render.jq` shows `… working`.
- `akern → session file`: the line, as a user message.
- `akern → render.jq`: a `message` event with that user message.
- `akern → render.jq`: a `turn_start` event, turn 1.
- `akern` launches the provider (`$AKERN_HOME/providers/NAME`, else
  `$AKERN_HOME/system/providers/NAME`).
- `akern → provider`: the request on its stdin (model, system prompt, messages,
  tool schemas, params), then EOF.
- `provider → akern → render.jq`: `text_delta`, `tool_use_start` and
  `tool_use_delta` events, passed through unchanged. `render.jq` prints the
  text.
- `provider → akern`: a `result` line, the whole assistant message with
  `stop: tool_use`. `akern` keeps it rather than passing it on.
- The provider exits.
- `akern → session file`: the assistant message, byte for byte.
- `akern → render.jq`: a `message` event with it.
- `akern → render.jq`: a `tool_start` event for `read`. The calls start only
  now, after the provider has exited.
- `akern` runs `read` (the first tool call) itself.
- `akern → render.jq`: a `tool_end` event for `read`.
- `akern → render.jq`: a `tool_start` event for `exec`.
- `akern` runs `exec` `ls src` (the second tool call) in its working directory.
- `ls → akern`: its stdout and stderr. `ls` exits.
- `akern → render.jq`: a `tool_end` event for `exec`.
- `akern → session file`: both results in one user message, in call order
  (`read`, then `exec`).
- `akern → render.jq`: a `message` event with it.
- `akern → render.jq`: a `turn_end` event, turn 1, with its token usage.
- `akern → render.jq`: a `turn_start` event, turn 2.
- `akern` launches a new provider.
- `akern → provider`: the request, now three messages, then EOF.
- `provider → akern → render.jq`: `text_delta` events. `render.jq` prints the
  answer.
- `provider → akern`: a `result` line with `stop: end_turn`. The provider exits.
- `akern → session file`: the answer.
- `akern → render.jq`: a `message` event with it.
- `akern → render.jq`: a `turn_end` event, turn 2.
- `akern → render.jq`: a `run_end` event, `done`: nothing else is waiting.
  `render.jq` shows `› waiting for input`.
- `akern` waits for the next line. At EOF (Ctrl-D) it exits, then `render.jq`
  and `akern-cli`.

The model runs only in a new provider process, one per turn. A line arriving
while idle starts one; so does the last running call of a turn finishing, and
results that finish together share one message. A `"wait": false` call would
be answered `tool_pending` at once, and its later exit would wake the model
again with a `job_result` ([kernel.md](kernel.md#sequences)).
