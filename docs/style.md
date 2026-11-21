# Style

How the C in `akern/`, `shared/`, `providers/` and `tools/` is written. These
are defaults, not laws: break one when following it would make the code harder
to read, and say why in the code.

## Names

**Most function names have a verb that says what the call does.** A reader
should know from the name alone whether a call answers a question, returns
something, does I/O, fills a buffer or writes to stdout. A module prefix comes
first, then the verb: `session_read_message`, `turns_put_status`.

| Verb | Meaning | Examples |
|---|---|---|
| `is_`, `has_`, `was_` | a yes/no question; no side effects | `turns_is_quiescent`, `session_has_failed`, `provider_was_aborted` |
| `get_` | returns something that already exists; cheap, no I/O | `session_get_head_id`, `json_get_num` |
| `read_` | does I/O to produce its result | `session_read_context`, `provider_read_request` |
| `put_` | writes into a buffer the caller passes in | `json_put_string`, `turns_put_status` |
| `emit_` | writes a line to stdout | `output_emit`, `provider_emit_result` |
| `count_` | returns how many | `calls_count_running_jobs` |

Any other verb is fine when it says what happens: `spawn`, `reap`, `take`,
`advance`, `blame`, `render`, `decode`.

A noun alone (`session_context`) leaves the reader to guess whether it reads,
builds or returns, and an adjective alone (`messages_complete`) can read as a
command. Exceptions, kept because they are short and used everywhere:
conversions and comparisons (`buf_span`, `buf_cstr`, `span_eq`, `json_eq`), the
iterator pair `json_iter` / `json_next`, the `x` allocators, and pure functions
of their arguments like `earliest_deadline`.

## Layout

`clang-format` (21 or later) does the layout, from [.clang-format](../.clang-format): LLVM
style with 4-space indents, 100 columns, and the brace rules below. Run
`clang-format -i` on the files you change; what it produces is the style.

**Braces go on their own line** after a function and after control flow. A
`struct`, `union` or `enum` keeps its opening brace on the same line.

**A bracket that has to split closes on its own line**, at the indent of the
line that opened it, with its contents indented once: calls, signatures, braced
lists and `if`, loop and `switch` conditions alike. Parameters, arguments and
list items that don't fit on one line go one per line. (clang-format 23 still wraps a plain `if`
condition at an operator instead; a long condition is clearer as a named
`bool` anyway.)

```c
static int on_progress(
    void *clientp,
    curl_off_t dltotal,
    curl_off_t dlnow,
    curl_off_t ultotal,
    curl_off_t ulnow
)
{
```

**Never put an `if`, `for`, `while` or `else` and its body on one line.** A
single-statement body may skip the braces, but it goes on its own, indented
line, so the condition and the action read separately and a diff that changes
one does not touch the other.

```c
/* not this */
if (!p) die("out of memory");
for (size_t i = 0; i < n; i++) free(v[i]);

/* this */
if (!p)
    die("out of memory");
for (size_t i = 0; i < n; i++)
    free(v[i]);

while (json_next(&it, NULL, &block))
{
    record_media(block);
    count++;
}
```
