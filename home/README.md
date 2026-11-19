# ~/.akern

```
~/.akern/
  system/        owned by `make install`: replaced whole on every install, do not edit
    providers/   anthropic  openai  claude-code  mock
    skills/      NAME/SKILL.md
    render.jq    how `akern-cli` displays events
    system.md    system prompt: lists the skills above
    README.md    this file
  providers/     yours (optional): shadows system/providers by name
  render.jq      yours (optional): replaces system/render.jq
  system.md      yours (optional): replaces system/system.md
  model          the default provider/model, written by `akern-model`
```

Everything outside `system/` is yours. `make install` writes nothing there.
It removes only a provider, `render.jq` or `system.md` that is byte-identical
to the new built-in (a copy left by installs from before `system/`, so
removing it changes nothing), and prints each one. A development checkout's
`build/` has this same layout.

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
| system prompt | `system.md`, then `system/system.md` (or `-S FILE`) |

A wrapping provider can run the built-in one: `exec ~/.akern/system/providers/anthropic`.

## Things to know

- A file you copy out is frozen: later installs improve `system/` but not
  your copy. Delete it to go back to the built-in, and `diff` the two to see
  what changed upstream.
- `system/system.md` lists only the skills in `system/skills`. If you write
  your own `system.md`, list your skills in it too (copy the format from
  `system/system.md`). The model reads a skill's `SKILL.md` by path.
- Programs (`akern`, `akern-edit`, `akern-cli`, `akern-model`) are installed
  on `PATH`, not here. To replace one, put your version earlier on `PATH`.
