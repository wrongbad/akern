# ~/.akern

```
~/.akern/
  system/           owned by `make install`: replaced whole on every install, do not edit
    providers/      anthropic  openai  claude-code  mock
    skills/         NAME/SKILL.md
    render.jq       how `akern-cli` displays events
    system-prompt   prints the system prompt: base-prompt.md, system.md, the skills, AGENTS.md files
    base-prompt.md  how to work in akern, at the top of the system prompt
    tools.json      the tools the model is given: names, descriptions, input schemas
    README.md       this file
  providers/        yours (optional): shadows system/providers by name
  skills/           yours (optional): NAME/SKILL.md, listed first; shadows system/skills/NAME
  render.jq         yours (optional): replaces system/render.jq
  system-prompt     yours (optional): replaces system/system-prompt
  tools.json        yours (optional): replaces system/tools.json
  base-prompt.md    yours (optional): replaces system/base-prompt.md; empty, none
  system.md         yours (optional): your own text, after the base prompt
  model             the default provider/model, written by `akern-model`
```

Everything outside `system/` is yours. `make install` writes nothing there.
It removes only a provider, `render.jq`, `system-prompt`, `base-prompt.md` or `tools.json` that is byte-identical
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
| system prompt | the output of `system-prompt`, then of `system/system-prompt`, run once when `akern` starts (or `-S FILE`) |
| tools | `tools.json`, then `system/tools.json`, read once when `akern` starts; a JSON array of `{name, description, input_schema, program}`; `program` is what a call runs (for `shell`, `bash` or `sh`, given `-c COMMAND`). Reword a description, drop a tool, or point one at your own program; `name` must be `shell`, `job` or `edit`; `job` runs in the kernel and has no `program` |
| skill `NAME` | `skills/NAME/SKILL.md`, then `system/skills/NAME/SKILL.md`, as `system/system-prompt` lists them |

A wrapping provider can run the built-in one: `exec ~/.akern/system/providers/anthropic`.

## Things to know

- A file you copy out is frozen: later installs improve `system/` but not
  your copy. Delete it to go back to the built-in, and `diff` the two to see
  what changed upstream.
- `system/system-prompt` adds `system.md` to the rest rather than replacing it. To
  change the prompt itself, write your own executable `system-prompt`; it can run
  `system/system-prompt` and add to its output.
- Programs (`akern`, `akern-edit`,
  `akern-cli`, `akern-model`) are installed
  on `PATH`, not here. To replace one, put your version earlier on `PATH`.
