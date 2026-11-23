---
name: acp
description: Sub-agents that speak ACP (another coding agent, such as one the user names) with the akern-agent command — its flags, steering, permissions, continuing one, and examples. Read it before handing a task to an agent that is not akern.
---

# ACP sub-agents

`akern-agent` runs any agent that speaks the Agent Client Protocol, version 2,
the way `akern -p` runs an akern sub-agent (see the akern skill): you give it a
task, it works with none of your context, and its answer is the command's
output. Use it for agents other than akern; for akern itself, `akern -p` is
simpler. Which agents exist and the command that starts each is the user's to
say (their `AGENTS.md`, or ask); `akern-acp` is akern over ACP and is always
there.

```
akern-agent [-a AGENT_COMMAND] [-s SESSION.jsonl] [-C DIR] [-m MODEL] [-o ID=VALUE]... [--allow] PROMPT...
```

| Flag | Meaning |
|---|---|
| `-a COMMAND` | the command that starts the agent, run with `sh -c`. Needed for a new session; a continued one uses the command its file recorded |
| `-s SESSION.jsonl` | its session: created if missing, continued if it exists. Without `-s`, a throwaway file. Its directory must exist. One `akern-agent` at a time per file: a second exits 2 |
| `-C DIR` | its working directory; default, yours (a continued session keeps its own) |
| `-m MODEL` | the agent's model, by its id or name, as the agent lists them; a wrong one exits 2 and lists the choices |
| `-o ID=VALUE` | any other option the agent offers (`-o mode=plan`, `-o effort=low`), by id or category; a wrong one exits 2 and lists what there is |
| `--allow` | grant the agent's permission requests. Without it, the first one is refused and its work stops (exit 3) |
| `PROMPT...` | the task, as one argument |

Its stdout is the agent's final answer each time it finishes working, and
nothing else. It exits once the agent is idle with no background work left,
without waiting for its stdin to end.

| Exit | When |
|---|---|
| `0` | it finished |
| `1` | its work ended in an error, or the agent died; the reason (and the end of the agent's stderr) on stderr |
| `2` | bad arguments, the agent would not start or is not protocol 2, the session could not be created or continued, a bad `-m`/`-o`, or the session file is in use |
| `3` | it asked permission for something and was refused; stderr says what. Continue with `--allow` if it should go ahead |
| `130` | stopped by a signal |

While it runs, each line on its stdin is a message, the same lines `akern -p`
takes:

| Line | Effect |
|---|---|
| plain text | a message to the agent; the agent decides whether it steers the current work or waits its turn |
| `{"type":"send","when":"now","content":"…"}` | cancels what it is doing, then sends the message |
| `{"type":"abort"}` | cancels what it is doing; it exits once the agent is idle |

## Permissions

Agents other than akern often ask before running commands or editing files.
Decide before you start: pass `--allow` when the task is one you would do
yourself without asking. Otherwise leave it out, and an exit 3 tells you what
it wanted; ask the user if it is not clearly part of the task, then continue
the same session with `--allow`.

## Examples

Delegate and keep working (`eof: false` keeps its stdin open for steering):

```json
{"command": "mkdir -p agents && akern-agent -a 'my-agent --acp' --allow -s agents/tests.jsonl 'Run the test suite, fix any failing tests, and reply with a list of what you changed and why.'",
 "wait": 0, "eof": false}
```

A long or quote-heavy task: write it to a file first, as with `akern -p`:

```json
{"command": "cat > agents/review.task.md", "stdin": "Review src/parser.c for …\n"}
{"command": "akern-agent -a 'my-agent --acp' -s agents/review.jsonl \"$(cat agents/review.task.md)\"", "wait": 0}
```

Steer it (`job` with `stdin`, each line ending in a newline):

```json
{"id": "<its call id>", "stdin": "Also check the Windows build.\n"}
{"id": "<its call id>", "stdin": "{\"type\":\"send\",\"when\":\"now\",\"content\":\"Stop: the bug is in lexer.c.\"}\n"}
```

Stop it: `{"id": "<its call id>", "signal": "TERM"}`. Wait for it a while:
`{"id": "<its call id>", "wait": 120}`.

Continue it later, with what it already knows: the same `-s` and a new prompt;
`-a` is not needed again. That is also how you answer a refused permission or
a question it ended with:

```json
{"command": "akern-agent --allow -s agents/tests.jsonl 'Yes, delete build/. Carry on.'"}
```

Watch its progress: the session file holds every protocol line,
`{"in": …}` from the agent and `{"out": …}` to it, after a header line. Its
text so far:

```
jq -rj 'select(.in.params.update.sessionUpdate == "agent_message_chunk") | .in.params.update.content.text // empty' agents/tests.jsonl
```

Its state (`running`, `requires_action`, `idle`):
`jq -c 'select(.in.params.update.sessionUpdate == "state_update") | .in.params.update' agents/tests.jsonl | tail -n 1`.
The agent's own log is `agents/tests.jsonl.stderr`.
