/* output: everything the kernel writes to stdout (akern.md §4): the
   event stream or, with -p, only each run's answer, with a failed run's error on stderr. Owns
   formatting events, the stdout backlog and whether the frontend has fallen too far behind,
   and the last blocking flush. Knows nothing of the modules that write to it; what an event
   says is theirs. */

#ifndef OUTPUT_H
#define OUTPUT_H

#include "loop.h"

typedef struct output {
    buf pending;
    bool blocking, dead, answers_only;
} output;

/* Starts output: events, or with answers_only (-p) each run's answer. */
void output_open(output *out, bool answers_only);
/* Polls stdout while bytes are pending. */
void output_watch(output *out, poll_set *set);
/* True while unwritten stdout is over its cap. */
bool output_is_backlogged(const output *out);
/* At exit: writes whatever is pending, blocking. */
void output_flush(output *out);
/* With -p: a run's answer on stdout, its error on stderr. */
void output_write_answer(output *out, span answer, json error);

/* Emits one event, formatted as by json_printf. */
void output_emit(output *out, const char *fmt, ...);
/* Emits one JSON line as given. */
void output_emit_raw(output *out, json line);
/* Emits a notice event from a source. */
void output_emit_notice(output *out, const char *source, span message);
/* Emits an error event saying where it happened. */
void output_emit_error(output *out, const char *where, span message);

#endif
