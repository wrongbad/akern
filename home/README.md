# ~/.akern

```
~/.akern/
  system/        owned by `make install`: replaced whole on every install, do not edit
    providers/   anthropic  openai  claude-code  mock
    skills/      NAME/SKILL.md
    render.jq    how `akern-cli` displays events
    prompt       prints the system prompt: system.md, the skills, AGENTS.md files
    README.md    this file
  providers/     yours (optional): shadows system/providers by name
  skills/        yours (optional): NAME/SKILL.md, listed first; shadows system/skills/NAME
  render.jq      yours (optional): replaces system/render.jq
  prompt         yours (optional): replaces system/prompt
  system.md      yours (optional): your own text, at the top of the system prompt
  model          the default provider/model, written by `akern-model`
```

Everything outside `system/` is yours. `make install` writes nothing there.
It removes only a provider, `render.jq` or `prompt` that is byte-identical
to the new built-in (a copy that changes nothing), and prints each one.
`make uninstall` removes `system/` and the programs, and leaves your files. A
development checkout's `build/` has this same layout.

## Overriding

The user layer is looked up first, then `system/`. To change a built-in,
copy it out and edit the copy:

```sh
mkdir -p ~/.akern/providers
cp ~/.akern/system/providers/anthropic ~/.akern/providers/anthropic
```

| Thing | Looked up |
|---|---|
| provider `NAME` | `providers/NAME`, then `system/providers/NAME`; first executable file wins. `akern-model` lists the union, a name once |
| `render.jq` | `render.jq`, then `system/render.jq` |
| system prompt | the output of `prompt`, then of `system/prompt`, run once when `akern` starts (or `-S FILE`) |
| skill `NAME` | `skills/NAME/SKILL.md`, then `system/skills/NAME/SKILL.md`, as `system/prompt` lists them |

A wrapping provider can run the built-in one: `exec ~/.akern/system/providers/anthropic`.

## Things to know

- A file you copy out is frozen: later installs improve `system/` but not
  your copy. Delete it to go back to the built-in, and `diff` the two to see
  what changed upstream.
- `system/prompt` adds `system.md` to the rest rather than replacing it. To
  change the prompt itself, write your own executable `prompt`; it can run
  `system/prompt` and add to its output.
- Programs (`akern`, `akern-edit`, `akern-cli`, `akern-model`) are installed
  on `PATH`, not here. To replace one, put your version earlier on `PATH`.
