# akern

An agent kernel: a truly minimal agent harness in C99. 

## Quick start

```sh
# install default env vars: PREFIX=/usr/local AKERN_HOME=~/.akern
make install                        
akern-cli "what does this repo do?"
```

Inspired by Unix: write programs that do one thing and do it well. And following the popular conventions in unix programs, we design this system as a collections of executables using stdio pipes to plug and play with existing tools like jq, and user extensions written in any language. "Executable" here includes scripts and anything that can be launched from the command line.

The kernel `akern` runs the core agent event loop, and delegates out to external executables for model requests, system prompt building, and the user facing CLI is offloaded to a wrapper script. This makes behaviors extremely easy to customize and extend. The communication protocols are text or jsonl over stdin/stdout pipes, enabling plugins to be written as simple bash and jq scripts.

Why C99? It's a sort of fun aesthetic / nostalgic choice. For a tiny project like this, basically any language can be sufficiently fast and safe without any trouble. So I picked one that hasn't been ruined for me in the age of +15,487 lines LGTM pull requests. 

The goal is feature parity with the [pi](https://github.com/earendil-works/pi) coding harness, reached by composition of trivial user-replacable scripts and binaries.

In some ways we already exceed base pi capabilities. Concurrent tool calls and recursive sub-agent calls come for free with the subprocess design.

`akern` exposes exactly 3 tool calls to models: `exec`, `read`, and `write`. You could imaging we get by with only exec, since you can call `cat` and such. But we hold the stdin pipe handle for launched subprocesses, so we can only write to that in-process.

## Developing

```sh
make            # install to ./build/
make test       # run tests
./akern-dev.sh  # run akern-cli with AKERN_HOME=./build
```

| Target | Does |
|---|---|
| `make` | builds into `build/`, laid out like an installed `~/.akern` |
| `make test` | pytest in `build/venv` over `tests/`. |
| `make asan` | the same tests against `build/asan/`, built with AddressSanitizer and UBSan. |
| `make lines` | counts the kernel's lines |
| `make install` | [tools.md §3](docs/tools.md#install) |
| `make clean` | removes `build/` |

Not yet built: `akern-tui`, the session commands (`fork`, `tree`,
`compact`, `sessions`).

## Installing

`make install` copies the programs to `PREFIX/bin` and replaces
`AKERN_HOME/system` whole with `build/system/`, so nothing dropped from a
release lingers. Never edit `system/`: put your own `providers/`, `skills/`,
`render.jq` or `prompt` in `AKERN_HOME` itself, where they are looked up first
and never overwritten. A `system.md` there is added to the top of the system
prompt. `make uninstall` removes the programs and `system/`, and leaves your
files. `~/.akern/system/README.md` describes the layout.

See [tools.md §3](docs/tools.md#install) for details.

## Docs

| Doc | Covers |
|---|---|
| [docs/design.md](docs/design.md) | goals, principles, costs and security posture |
| [docs/akern.md](docs/akern.md) | the kernel process: environment, arguments, stdin commands, stdout events, session files |
| [docs/turn.md](docs/turn.md) | one typed prompt with two tool calls, step by step: the processes and events |
| [docs/kernel.md](docs/kernel.md) | `src/`: each module's state and API, delivery modes, jobs, the dependency graph |
| [docs/providers.md](docs/providers.md) | `providers/`: the request, the event stream, errors, the message schema; anthropic, claude-code, openai, mock |
| [docs/tools.md](docs/tools.md) | `tools/` and `skills/`: `exec`, `read`, `write`, programs on `PATH`, skills, the `akern-cli` front end |
| [docs/pi-parity.md](docs/pi-parity.md) | every pi feature mapped to the mechanism that provides it here |
| [docs/roadmap.md](docs/roadmap.md) | remaining work and open questions |
