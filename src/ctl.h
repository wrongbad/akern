/* ctl: the kernel's control channel (protocols.md §3, akern-loop.md): commands and prompt
   lines on stdin, and everything written to stdout, events or, with -p, answers. Owns parsing
   and replying to commands, the stdout sink every module writes its events to, the stdout
   backlog and whether the frontend has fallen too far behind, and holding an append that names
   a parent until a turn boundary. What a command does to the conversation belongs to the
   driver, calls, inbox and session. */

#ifndef CTL_H
#define CTL_H

#include "loop.h"

typedef struct sink {
    buf  pending;
    bool blocking, dead, answers_only;
} sink;

typedef struct ctl {
    sink            out;
    buf             stdin_pending;
    bool            stdin_eof, skip_rest_of_line;
    struct deferred_entry *deferred;
    size_t          ndeferred, capdeferred;
    const config   *config;
    struct inbox   *inbox;
    struct driver  *driver;
    struct calls   *calls;
    struct session *session;
} ctl;

void ctl_open(ctl *c);
void ctl_watch(ctl *c, poll_set *set);
void ctl_watch_output(ctl *c, poll_set *set);
bool ctl_backlogged(const ctl *c);
bool ctl_eof(const ctl *c);
void ctl_flush(ctl *c);
void ctl_append_deferred_entries(ctl *c);

void ctl_print(sink *out, span text);
void event(sink *out, const char *fmt, ...);
void event_raw(sink *out, json line);
void event_notice(sink *out, const char *source, span message);
void event_error(sink *out, const char *where, span message);

#endif
