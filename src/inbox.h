/* inbox: what waits to enter the conversation: prompts and injected content from the
   frontend, and the results of finished jobs. Owns queueing items and handing them out by
   delivery mode in arrival order, telling items queued since a mark from older ones, and
   giving undelivered prompts back when a run is aborted.
   When a delivery happens, and writing it to the session, are the driver's.
   delivery mode (enum when) = the point at which a queued item may enter the conversation
   (kernel.md), named by ctl's commands and used by the driver. */

#ifndef INBOX_H
#define INBOX_H

#include "json.h"

enum when { WHEN_NOW = 1, WHEN_TURN = 2, WHEN_IDLE = 4, WHEN_DEFERRED = 8 };
enum source { FROM_PROMPT, FROM_JOB, FROM_INJECT };

typedef struct inbox {
    struct item **items;
    size_t        nitems, capitems, nadded, mark;
    struct sink  *sink;
} inbox;

const char *when_name(enum when when);
bool        when_parse(span name_or_json_string, enum when *when);

void   inbox_add(inbox *in, enum source source, enum when when, json content);
bool   inbox_due(const inbox *in, unsigned when_mask);
void   inbox_mark(inbox *in);
bool   inbox_due_since_mark(const inbox *in, unsigned when_mask);
size_t inbox_take(inbox *in, unsigned when_mask, json_list *blocks);
void   inbox_return_prompts(inbox *in, unsigned when_mask, json_list *contents);
void   inbox_defer_jobs(inbox *in);
size_t inbox_take_jobs(inbox *in, json_list *blocks);
size_t inbox_size(const inbox *in);

#endif
