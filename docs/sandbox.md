# Sandbox and keys

How a whole agent runs confined, and how its model calls are paid for without
an API key inside the confinement. **Design only: nothing here is built.** It is
written down to check that the kernel fits a complete, safe setup as it is.
It does: the plan below needs no kernel change ([§5](#5-what-the-kernel-relies-on)).

Every piece is optional, and each one is useful alone
([§6](#6-pieces-alone)).

## 1. The problem

An agent runs with your privileges. Whatever you can read, the model can read
too:

- the kernel passes its environment to every child, tool calls included
  ([akern.md §1](akern.md#set-for-children)), so `env` shows any
  `ANTHROPIC_API_KEY` the kernel was started with;
- a process can read the environment of other processes run by the same user (`ps -E`);
- any file you can read: `~/.bashrc`, a key file, the login Keychain through
  `security find-generic-password`.

Moving a key out of `~/.bashrc` helps only once the agent runs with less
authority than you. Keeping keys away from the agent and sandboxing the agent
are one problem.

## 2. The shape

Two boundaries: a container around the agent, and a key proxy outside it that
is the only holder of the keys.

```
your Mac                                    container (one per agent run)
────────                                    ─────────────────────────────
front end ─── stdin/stdout pipes ─────────▶ akern ─── provider ─────┐
(akern-cli; an editor via akern-acp)          │                     │ HTTP,
                                              └── tools, jobs,      │ session token
                                                  sub-agents        │
key proxy ◀─────────────────────────────────────────────────────────┘
  │  real key, from the Keychain
  ▼
api.anthropic.com, openrouter.ai

project directory ◀──── mount ────▶ /work
```

| Piece | Holds | Runs |
|---|---|---|
| launcher | nothing long-lived | on your Mac: asks the proxy for a token, starts the container with it, revokes the token when the container exits |
| container | the project mount, a session token | the whole agent: front end or `akern-acp`, the kernel, providers, tools, sub-agents |
| key proxy | the real keys | on your Mac, outside every container |

What the agent can reach: the project directory, the container's own
filesystem, the network, and the proxy with its token. What it cannot reach:
your home directory, the Keychain, the keys.

## 3. The key proxy

A small HTTP server on your Mac. For each request:

1. Accept plain HTTP on the container network only, never on `0.0.0.0`.
2. Look up the session token from the request's `x-api-key` or
   `authorization` header. Unknown or revoked: 401.
3. Pick the upstream by path prefix from a fixed table, and refuse any other
   prefix:

   | Prefix | Upstream | Header set |
   |---|---|---|
   | `/anthropic/` | `https://api.anthropic.com/` | `x-api-key: <key>` |
   | `/openrouter/` | `https://openrouter.ai/api/` | `authorization: Bearer <key>` |

4. Replace the token with the real key, send the request upstream over TLS, and
   stream the response back byte for byte (server-sent events included).

Keys live in the Keychain
(`security add-generic-password -s akern.anthropic -a "$USER" -w`), read by the
proxy at startup. No key is in any shell rc file or in any environment the
agent can see.

**The token is a credential for one agent run.** The launcher gets a fresh
random token for each container and the proxy revokes it when the container
exits, so a token that leaks is worthless after cleanup. The proxy keeps a
table of tokens with each one's allowed prefixes. It may also count usage per
token and stop a token at a budget; spend caps on the keys themselves (the
Anthropic Console, OpenRouter) cover what a token can spend while it is live.

**It is an HTTP proxy, not a provider.** The other option was to keep
providers on the Mac and carry the provider protocol across the container
boundary. Rejected:

- a provider is started per turn, in the kernel's working directory, with its
  environment, and aborted with `SIGTERM` to its process group
  ([providers.md](providers.md)); all of that would have to be rebuilt
  over a socket;
- the `claude-code` provider runs the `claude` program, which would then run
  outside the sandbox;
- it would tie the proxy to akern's protocol version.

At the HTTP level the proxy knows no API beyond two header names, providers stay
unchanged, and anything else in the container that calls these APIs works
through it too: an SDK script the agent writes, `curl`, `claude`. What does belong in the
proxy is what concerns the account rather than the conversation: which key,
which upstreams, usage per token, budgets.

## 4. The container

On macOS 26 on Apple silicon, Apple's open-source `container` tool runs each
container in its own lightweight virtual machine, from ordinary OCI images,
with no privileged daemon and no Docker Desktop. Any other runtime works the
same way (OrbStack, Colima, Docker, Podman, `bwrap` on Linux): the design uses
only "run a process in an image, with one directory mounted and stdio piped".

- **Image:** a Containerfile with the build tools, the libcurl headers, `jq`,
  `git`, and akern built and installed inside. Rebuilding it is how
  dependencies are added; nothing is installed on the Mac.
- **Run:** the launcher starts the agent with the project as the only mount:

  ```bash
  container run --rm -i -v "$PWD:/work" -w /work \
      -e ANTHROPIC_BASE_URL=http://<host>:<port>/anthropic \
      -e ANTHROPIC_API_KEY="$token" \
      -e OPENAI_BASE_URL=http://<host>:<port>/openrouter/v1 \
      -e OPENAI_API_KEY="$token" \
      akern akern-cli
  ```

  The providers already read these variables
  ([providers.md](providers.md)), and libcurl takes an `http://` base as
  readily as `https://`.
- **State:** with `--rm` the container's filesystem is thrown away on exit.
  Sessions that should outlive a run go in a mounted per-project state directory
  (`AKERN_HOME`, `XDG_STATE_HOME`); everything else is cleaned up for free.
- **Sub-agents** are `akern -p` or `akern-agent` run through `shell`
  ([design.md](design.md#deliberately-outside-the-kernel)), so they start in
  the same container, inherit the same environment and use the same token.
- **Editors:** the editor's ACP command is the launcher, which runs
  `container run -i … akern-acp`; the stdio pipes cross the boundary like any
  other ([design.md](design.md#security-posture)).

## 5. What the kernel relies on

Each is already true; none is added for this design.

| Fact | Where |
|---|---|
| The kernel's only channels are its stdin and stdout, so a front end on the Mac drives a kernel in a container through `container run -i` | [design.md](design.md#seams) |
| Children inherit the kernel's environment unchanged, so the providers and sub-agents in the container all get the proxy URL and token | [akern.md §1](akern.md#set-for-children) |
| Providers take their base URL from the environment | [providers.md](providers.md) |
| One process tree with one working directory, and no listening endpoint, so nothing reaches into the container from outside | [design.md](design.md#security-posture) |
| A sandbox that fails to start starts no kernel | [design.md principle 10](design.md#principles) |

The kernel holds no keys, strips nothing from children's environments, and
knows nothing about containers. A token in the agent's environment is fine:
it works only through the proxy, only for this run.

## 6. Pieces alone

| Setup | Gives |
|---|---|
| neither (today) | keys in the environment; the agent can read them and anything else of yours |
| container, keys passed with `-e` | your home directory and Keychain are out of reach; the key itself is visible to the agent |
| proxy only, on the Mac, providers pointed at it | no key in any shell rc file or environment; a process running as you can still read the Keychain or ask the proxy for a token |
| container and proxy | the agent can spend through its token while the run lasts, and can read neither the keys nor your files |

## 7. Alternatives considered

| Option | Why not, for now |
|---|---|
| Seatbelt (`sandbox-exec`, or Anthropic's `sandbox-runtime`) around each tool call, through the tool's `program` ([tools.md](tools.md#1-built-in-tools)) | cheaper per call, and fits the existing wrapper seam, but runs as your user, so a gap in the profile is a hole; no reproducible dependency setup or cleanup |
| a separate macOS user | a real kernel-enforced boundary, but shared project directories need groups and ACLs, the home directory must be closed (`chmod 700 ~`), and the network stays open |
| a Keychain item that trusts only the provider binary (`-T`) | prompts again after every rebuild; trusting `security` reopens the hole; a provider that reads its base URL from the environment would send the key wherever it points |
| the kernel stripping `*_API_KEY` from tool calls | a same-user process can still read the kernel's environment and your files; unnecessary once keys never enter the container |

## 8. Open questions

Settle before building:

- **Reaching the host:** at what address a container reaches the Mac under
  Apple `container`, and binding the proxy to that interface only.
- **akern on Linux:** it has only been built on macOS so far; the image needs
  it to build and pass its tests on Linux.
- **The `claude-code` provider:** `claude` authenticates with an OAuth token,
  not an API key. Either the proxy injects that token too, or this provider is
  left out at first and its token lives in the container.
- **Egress:** the container can still reach the internet, so an agent cannot
  take a key but can upload the project. Restricting the container's network
  to the proxy and a package mirror is a later step.
- **Usage counting:** whether the proxy reads usage from the response stream
  (it then knows the APIs' usage fields) or leaves budgets to the keys' own
  spend caps.
