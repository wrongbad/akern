/* sink: everything the kernel writes to stdout (akern.md §4): the
   event stream or, with -p, only each run's answer, with a failed run's error on stderr. Owns
   formatting events, the stdout backlog and whether the frontend has fallen too far behind,
   and the last blocking flush. Knows nothing of the modules that write to it; what an event
   says is theirs. */

#ifndef SINK_H
#define SINK_H

#include "loop.h"

typedef struct sink {
    buf  pending;
    bool blocking, dead, answers_only;
} sink;

void sink_open(sink *out, bool answers_only);
void sink_watch(sink *out, poll_set *set);
bool sink_backlogged(const sink *out);
void sink_flush(sink *out);
void sink_answer(sink *out, span answer, json error);

void event(sink *out, const char *fmt, ...);
void event_raw(sink *out, json line);
void event_notice(sink *out, const char *source, span message);
void event_error(sink *out, const char *where, span message);

#endif
