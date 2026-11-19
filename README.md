# akern

An agent kernel: a minimal agent harness in C99. The kernel runs the
model/tool loop, concurrently, and brokers the model's reads, writes and
executions to the OS the way a Unix kernel or a GPU driver moves bytes: as
opaque data it schedules and delivers but never interprets. Everything
else — the model provider, every tool, every policy, every UI — is a separate
program that speaks JSON lines over a pipe.

The target is feature parity with the [pi](https://github.com/earendil-works/pi)
coding harness, reached by composition instead of by a plugin API: if you can
write an executable in any language, you can extend it. No threads, no
libraries: parallel tool calls, background jobs, steering that can interrupt a
turn, and sub-agents all come from processes and one `poll(2)` loop.

## Quick start

```sh
make            # everything under build/, laid out as an AKERN_HOME
make test      # scenario tests against the mock provider
./akern.sh      # no env vars needed; the first time, asks for a model and saves it in build/model
./akern.sh "what does this repo do?"
```

`./akern.sh` sets `AKERN_HOME=build` and puts `build/bin` first on `PATH`
itself, so it runs from a fresh checkout with no environment setup. With no
`build/model` it lists the providers (marking the ones that cannot be used,
and why) and their models, and saves your choice. To change it later, run
`./akern.sh` with `AKERN_MODEL=provider/model` for one run, or run
`build/bin/akern-model` (with `AKERN_HOME=build`) to pick again, or pass it
`provider/model` to set it without asking.

| Target | Does |
|---|---|
| `make` | `build/bin/` (`akern-loop`, `akern-edit`, `akern`, `akern-model`), `build/providers/`, `build/skills/`, `build/render.jq`, `build/system.md` |
| `make test` | pytest in `build/venv` over `tests/`. After every test, `tests/invariants.py` checks each session and event log: every `tool_use` answered in the next message in call order, every `tool_pending` closed by one `job_result`, no `tool_use` in an aborted message, one provider stream at a time, every `tool_start` ended, `run_end.error` set exactly when the reason is `error`. `tests/bin/gate PATH` blocks until the test creates PATH, so a test decides when each call finishes |
| `make asan` | the same tests against `build/asan/`, built with AddressSanitizer and UBSan; fails on any report (`build/asan/sanitizer.*`). The two trees share no objects, so switching needs no `make clean` |
| `make lines` | counts the kernel's lines |
| `make install` | [tools.md §3](docs/tools.md#install) |
| `make clean` | removes `build/` |

Not yet built: a richer line mode, the session commands (`fork`, `tree`,
`compact`, `sessions`), `akern-pkg`.

## Docs

| Doc | Covers |
|---|---|
| [docs/design.md](docs/design.md) | goals, principles, costs and security posture |
| [docs/akern-loop.md](docs/akern-loop.md) | the kernel process: environment, arguments, stdin commands, stdout events, session files |
| [docs/kernel.md](docs/kernel.md) | `src/`: each module's state and API, delivery modes, jobs, the dependency graph |
| [docs/providers.md](docs/providers.md) | `providers/`: the request, the event stream, errors, the message schema; anthropic, claude-code, mock |
| [docs/tools.md](docs/tools.md) | `tools/` and `skills/`: `exec`, `read`, `write`, programs on `PATH`, skills, the `akern` front end |
| [docs/pi-parity.md](docs/pi-parity.md) | every pi feature mapped to the mechanism that provides it here |
| [docs/roadmap.md](docs/roadmap.md) | remaining work and open questions |
