---
name: akern
description: Sub-agents with the akern command — its flags, steering it while it runs, and examples. Read it before handing a task to another agent.
---

# Sub-agents

A sub-agent is another `akern`, run with `shell` like any program. It starts
with none of your context and reports once, when it is done.

```
akern -p [-s SESSION.jsonl] [-m PROVIDER/MODEL] [-S SYSTEM.md] [-P KEY=VALUE]... [PROMPT...]
```

| Flag | Meaning |
|---|---|
| `-p` | print mode: its stdout is the final answer of each run, and it exits once it is done, without waiting for its stdin to end. Always pass it |
| `-s SESSION.jsonl` | its conversation, one JSON entry per line; created if missing, continued if it exists. Without `-s`, a throwaway file. Its directory must exist. One `akern` at a time per file: a second exits 2 |
| `-m PROVIDER/MODEL` | another model; default, yours (`$AKERN_MODEL`). List a provider's models with `$AKERN_HOME/system/providers/NAME --models`; the providers are the files in `$AKERN_HOME/providers` and `$AKERN_HOME/system/providers` |
| `-S SYSTEM.md` | its system prompt, read from the file, instead of the usual one (how to work in akern, the skills, the project's `AGENTS.md`), so include what it needs of those |
| `-P KEY=VALUE` | a provider parameter, repeatable: `-P effort=low` for a cheaper run, `-P max_tokens=4000` |
| `PROMPT...` | the task, as one argument |

Exit 0, or 1 when its last run ended in an error (the message on stderr), 2
for bad arguments or a session in use. It inherits your environment and
working directory. Nesting is limited: with the defaults, three levels of
sub-agents below the first `akern`.

While it runs, each line on its stdin is a message:

| Line | Effect |
|---|---|
| plain text | a message it reads at its next step |
| `{"type":"send","when":"now","content":"…"}` | a message that interrupts what it is doing |
| `{"type":"abort"}` | ends its current run; its own jobs keep running, and with `-p` it exits once they are done |

## Examples

Delegate and keep working (`eof: false` keeps its stdin open for steering):

```json
{"command": "mkdir -p agents && akern -p -s agents/tests.jsonl 'Run the test suite in this repository, fix any failing tests, and reply with a list of what you changed and why.'",
 "wait": 0, "eof": false}
```

A long or quote-heavy task: write it to a file first, then pass it whole:

```json
{"command": "cat > agents/review.task.md", "stdin": "Review src/parser.c for …\n(any text, quotes included)\n"}
{"command": "akern -p -s agents/review.jsonl \"$(cat agents/review.task.md)\"", "wait": 0}
```

A quick question you wait for (no `wait`: the call returns its answer):

```json
{"command": "akern -p -m claude-code/haiku 'In one line: what does the -z flag of sort do?'"}
```

Several at once: put the calls in one reply, each with its own `-s`; each
answer arrives as its own job result.

```json
{"command": "akern -p -s agents/a.jsonl 'Summarize docs/a.md in five bullets.'", "wait": 0}
{"command": "akern -p -s agents/b.jsonl 'Summarize docs/b.md in five bullets.'", "wait": 0}
```

Steer it (`job` with `stdin`, each line ending in a newline):

```json
{"id": "<its call id>", "stdin": "Also check the Windows build.\n"}
{"id": "<its call id>", "stdin": "{\"type\":\"send\",\"when\":\"now\",\"content\":\"Stop: the bug is in lexer.c, not parser.c.\"}\n"}
```

Stop it: `{"id": "<its call id>", "signal": "TERM"}`; it stops its own
sub-agents and jobs, and its result still arrives. Wait for it a while:
`{"id": "<its call id>", "wait": 120}`.

Watch its progress: its session file grows one line per message, e.g.
`tail -n 3 agents/tests.jsonl | cut -c1-400`, or its text so far with
`jq -r 'select(.type=="message") | .message.content[]? | select(.type=="text") | .text' agents/tests.jsonl`.

Continue it later with what it already knows: run `akern -p` again with the
same `-s` and a new prompt. That is also how you answer a sub-agent that ends
with a question (before deleting or pushing something, say) instead of a
result: `akern -p -s agents/tests.jsonl 'Yes, delete build/. Carry on.'`.
