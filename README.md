# akern

An agent kernel: a minimal agent harness in C99. The kernel does one thing: it runs the
model/tool loop, concurrently. Everything else — the model provider, every
tool, every policy, every UI — is a separate program that speaks JSON lines
over a pipe.

The target is feature parity with the [pi](https://github.com/earendil-works/pi)
coding harness, reached by composition instead of by a plugin API: if you can
write an executable in any language, you can extend it.

```sh
echo "what does this repo do?" | akern-loop -m anthropic/claude-opus-5-5 | jq --unbuffered -rj -f build/render.jq
```

No threads, no libraries. Parallel tool calls, background jobs, steering that
can interrupt a turn, and sub-agents all come from processes and one `poll(2)`
loop.

## Status

First implementation of the whole design:

- `src/`: the kernel, `akern-loop`
- `providers/`: Anthropic and a scriptable mock
- `tools/`: `akern`, the front end for people (an optional first prompt, then
  each line typed, which steers the agent while it works), and `akern-edit`,
  which people, scripts and the model run directly (the model reads and writes
  files with its `read` and `write` tools); a sub-agent is just
  `akern-loop -p "task"` run with `wait: false` and `stdin_open`, steered with
  `write` to its job
- `skills/`: the `files` and `akern` skills, which `make` lists in `system.md`, the default system prompt
- `render.jq`: how `akern "prompt"` shows the event stream; edit it to change the display

Like an OS kernel's core syscalls, the kernel gives the model `exec`, `read`
and `write`, over files and jobs; everything else is a command and a
`SKILL.md`. Not yet built: a richer line mode, the session commands (`fork`, `tree`,
`compact`, `sessions`), `akern-pkg`.

```sh
make            # everything goes under build/, laid out as an AKERN_HOME
make smoke      # scenario tests against the mock provider
AKERN_MODEL=anthropic/claude-opus-5-5 ANTHROPIC_API_KEY=… ./akern.sh "what does this repo do?"
```

Read the docs in order:

| Doc | What it settles |
|---|---|
| [docs/design.md](docs/design.md) | Goals, principles, architecture, what is and is not in the kernel |
| [docs/protocols.md](docs/protocols.md) | The contracts: messages, session log, provider, tools, asking the user |
| [docs/concurrency.md](docs/concurrency.md) | Calls, pending calls and jobs, the inbox, delivery modes, preemption, sub-agents, invariants |
| [docs/akern-loop.md](docs/akern-loop.md) | Reference: environment, arguments, stdin commands, stdout events |
| [docs/kernel.md](docs/kernel.md) | How the C kernel is built: worker table, driver, poll loop, testing concurrency |
| [docs/pi-parity.md](docs/pi-parity.md) | Every pi feature mapped to the mechanism that provides it here |
| [docs/roadmap.md](docs/roadmap.md) | Milestones and the open questions still to decide |

The protocols and concurrency docs are the real design. The kernel stays small
because the contracts carry the weight; get those right and the C follows.
