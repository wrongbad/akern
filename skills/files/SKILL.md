---
name: files
description: Edit files with exact-match replacements that need no shell quoting, and view images and PDFs, from files or from programs.
---

# Files

`read` and `write` handle whole files. For changes inside a file, run
`akern-edit` with `exec`; each argument reaches it byte for byte, so
multi-line text needs no quoting or escaping.

| Goal | argv |
|---|---|
| Replace text | `["akern-edit", "src/app.c", "exact old text", "new text"]` |
| Replace every occurrence | `["akern-edit", "src/app.c", "old", "new", "--all"]` |

## Editing well

- Read the file first, and copy the old text exactly, indentation included.
- The old text must match exactly once. If it matches more than once, include
  a line or two of surrounding context to make it unique.
- `akern-edit` prints the changed region with line numbers; check it rather than
  reading the whole file again.
- Prefer several small edits to rewriting a large file.

## Images and PDFs

To see an image or a PDF, `read` it with `type` set to its MIME type:
`{"path": "shot.png", "type": "image/png"}`, `{"path": "spec.pdf", "type":
"application/pdf"}`. Without `type`, a binary file is refused. For media a
program produces, have it write to stdout and set `stdout_type` to the MIME
type on the exec call, e.g. a converter that writes PNG. Keep images under about
2,500 px on the long edge; larger ones are downscaled anyway, and very large
ones are dropped from the request.

Searching and listing are ordinary programs: `["rg", "-n", "pattern", "src"]`,
`["ls", "-la"]`, `["find", ".", "-name", "*.c"]`.
