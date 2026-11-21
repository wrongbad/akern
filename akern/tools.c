#include "tools.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool read_file(const char *path, buf *text)
{
    char chunk[8192];
    ssize_t n = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    while (fd >= 0 && (n = read(fd, chunk, sizeof chunk)) > 0)
        buf_put(text, chunk, (size_t)n);
    int error = errno;
    if (fd >= 0)
        close(fd);
    errno = error;
    return fd >= 0 && n == 0;
}

/* Drops the whitespace between tokens, so the request stays one line. */
static void put_compact(json_buf *out, json value)
{
    bool in_string = false, escaped = false;
    for (size_t i = 0; i < value.n; i++)
    {
        char ch = value.p[i];
        if (in_string)
        {
            in_string = escaped || ch != '"';
            escaped = !escaped && ch == '\\';
        }
        else if (ch == '"')
        {
            in_string = true;
        }
        else if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')
        {
            continue;
        }
        buf_putc(out, ch);
    }
}

/* The tools the kernel knows how to call (calls.c); tools.json picks among them. job runs in
   the kernel, the others as their program. */
static const char *const known_tools[] = {"shell", "job", "edit"};

static bool is_known(const char *name)
{
    for (size_t i = 0; i < LEN(known_tools); i++)
        if (strcmp(name, known_tools[i]) == 0)
            return true;
    return false;
}

static void add_tool(tools *ts, const char *path, size_t index, json tool)
{
    char *name = json_decode_dup(json_get(tool, "name"));
    char *program = json_decode_dup(json_get(tool, "program"));
    bool needs_program = name && strcmp(name, "job") != 0;
    const char *problem = !json_is(tool, '{')         ? "is not an object"
                          : !name                     ? "has no name"
                          : !is_known(name)           ? "is not shell, job or edit"
                          : !program && needs_program ? "has no program"
                          : tools_find(ts, name)      ? "is named twice"
                                                      : NULL;
    if (problem)
        die("%s: tool %zu %s", path, index + 1, problem);
    tool_def def = {.name = name, .program = program};
    VEC_PUSH(ts->defs, ts->ndefs, ts->capdefs, def);

    json_put_comma_unless_first(&ts->schemas);
    buf_putc(&ts->schemas, '{');
    bool first = true;
    jiter it = json_iter(tool);
    for (json key, value; json_next(&it, &key, &value);)
    {
        if (json_eq(key, "program"))
            continue;
        if (!first)
            buf_putc(&ts->schemas, ',');
        first = false;
        buf_putspan(&ts->schemas, key);
        buf_putc(&ts->schemas, ':');
        put_compact(&ts->schemas, value);
    }
    buf_putc(&ts->schemas, '}');
}

void tools_open(tools *ts, const char *home_dir)
{
    static const char *const layers[] = {
        "",
        "/system"
    }; /* the user's overlay, then what make install owns */
    buf text = {0}, path = {0};
    bool found = false;
    for (size_t i = 0; i < LEN(layers) && !found; i++)
    {
        path.n = 0;
        buf_printf(&path, "%s%s/tools.json", home_dir, layers[i]);
        found = read_file(path.p, &text);
        if (!found && errno != ENOENT)
            die("%s: %s", path.p, strerror(errno));
    }
    if (!found)
        die("no tools.json in %s or %s/system", home_dir, home_dir);
    json all = json_trim(buf_span(&text));
    if (!json_is_valid(all) || !json_is(all, '['))
        die("%s: not a JSON array", path.p);
    buf_putc(&ts->schemas, '[');
    jiter it = json_iter(all);
    size_t index = 0;
    for (json tool; json_next(&it, NULL, &tool); index++)
        add_tool(ts, path.p, index, tool);
    buf_putc(&ts->schemas, ']');
    buf_free(&text);
    buf_free(&path);
}

json tools_get_schemas(const tools *ts)
{
    return buf_span(&ts->schemas);
}

const tool_def *tools_find(const tools *ts, const char *name)
{
    for (size_t i = 0; i < ts->ndefs; i++)
        if (strcmp(ts->defs[i].name, name) == 0)
            return &ts->defs[i];
    return NULL;
}

void tools_put_names(const tools *ts, buf *text)
{
    for (size_t i = 0; i < ts->ndefs; i++)
    {
        const char *sep = i == 0 ? "" : i + 1 == ts->ndefs ? " and " : ", ";
        buf_printf(text, "%s%s", sep, ts->defs[i].name);
    }
}
