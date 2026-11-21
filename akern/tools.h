/* tools: the tool definitions in tools.json (tools.md §1). Owns finding and loading the file
   ($AKERN_HOME/tools.json, else $AKERN_HOME/system/tools.json), each tool's program (what a
   call runs), and the definitions sent to the provider, each without its program. How a
   call's input reaches the program is calls'. */

#ifndef TOOLS_H
#define TOOLS_H

#include "main.h"

typedef struct tool_def {
    char *name, *program; /* program is NULL for job, which runs in the kernel */
} tool_def;

typedef struct tools {
    tool_def *defs;
    size_t ndefs, capdefs;
    json_buf schemas;
} tools;

/* Loads tools.json; dies when there is none, it is malformed, or it names a tool the kernel
   cannot run. */
void tools_open(tools *ts, const char *home_dir);
/* The array sent with every request: each definition without its program. */
json tools_get_schemas(const tools *ts);
/* The definition with this name, or NULL. */
const tool_def *tools_find(const tools *ts, const char *name);
/* Writes the tool names as a list: "shell, job and edit". */
void tools_put_names(const tools *ts, buf *text);

#endif
