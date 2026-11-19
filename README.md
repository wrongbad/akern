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
make install                        # programs to /usr/local/bin, providers and skills to ~/.akern/system
                                    # elsewhere: make install PREFIX=$HOME/.local HOME_DIR=$HOME/.akern
akern-cli                           # the first time, asks for a model and saves it in ~/.akern/model
akern-cli "what does this repo do?"
```

`akern-model` picks the default model again, or sets it with
`akern-model provider/model`; `AKERN_MODEL=provider/model` overrides it for one
run.

## Developing

```sh
make            # everything under build/, laid out like an installed ~/.akern
make test       # scenario tests against the mock provider
./akern-dev.sh  # akern-cli against build/: AKERN_HOME=build, build/bin first on PATH
```

| Target | Does |
|---|---|
| `make` | `build/bin/` (`akern`, `akern-edit`, `akern-cli`, `akern-model`) and `build/system/` (`providers/`, `skills/`, `render.jq`, `system.md`, `README.md`), the layer `make install` copies to `~/.akern/system`. `build/` itself is the user layer: nothing in it is generated, so `build/model`, `build/providers/`, `build/render.jq` and `build/system.md` are yours |
| `make test` | pytest in `build/venv` over `tests/`. After every test, `tests/invariants.py` checks each session and event log: every `tool_use` answered in the next message in call order, every `tool_pending` closed by one `job_result`, no `tool_use` in an aborted message, one provider stream at a time, every `tool_start` ended, `run_end.error` set exactly when the reason is `error`. `tests/bin/gate PATH` blocks until the test creates PATH, so a test decides when each call finishes |
| `make asan` | the same tests against `build/asan/`, built with AddressSanitizer and UBSan; fails on any report (`build/asan/sanitizer.*`). The two trees share no objects, so switching needs no `make clean` |
| `make lines` | counts the kernel's lines |
| `make install` | [tools.md §3](docs/tools.md#install) |
| `make clean` | removes `build/` |

Not yet built: a richer line mode, the session commands (`fork`, `tree`,
`compact`, `sessions`), `akern-pkg`.

## Installing

`make install` copies the programs to `$(PREFIX)/bin` and replaces
`$(HOME_DIR)/system` whole with `build/system/`, so nothing dropped from a
release lingers. Never edit `system/`: put your own provider, `render.jq` or
`system.md` in `$(HOME_DIR)` itself, where it is preferred and never
overwritten; the install notes each one that shadows a built-in. A copy left by
an install from before `system/` that is byte-identical to the new built-in is
removed, since it changes nothing. `~/.akern/system/README.md` describes the
layout.

See [tools.md §3](docs/tools.md#install) for details.

## Docs

| Doc | Covers |
|---|---|
| [docs/design.md](docs/design.md) | goals, principles, costs and security posture |
| [docs/akern.md](docs/akern.md) | the kernel process: environment, arguments, stdin commands, stdout events, session files |
| [docs/kernel.md](docs/kernel.md) | `src/`: each module's state and API, delivery modes, jobs, the dependency graph |
| [docs/providers.md](docs/providers.md) | `providers/`: the request, the event stream, errors, the message schema; anthropic, claude-code, mock |
| [docs/tools.md](docs/tools.md) | `tools/` and `skills/`: `exec`, `read`, `write`, programs on `PATH`, skills, the `akern-cli` front end |
| [docs/pi-parity.md](docs/pi-parity.md) | every pi feature mapped to the mechanism that provides it here |
| [docs/roadmap.md](docs/roadmap.md) | remaining work and open questions |
