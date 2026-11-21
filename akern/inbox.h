/* inbox: what waits to enter the conversation: prompts and injected content from the
   frontend, and the results of finished jobs. Owns queueing items and handing them out by
   delivery mode in arrival order, telling items queued since a mark from older ones, and
   giving undelivered prompts back when a run is aborted.
   When a delivery happens, and writing it to the session, are turns'.
   delivery mode (enum when) = the point at which a queued item may enter the conversation
   (akern.md), named by input's commands and used by turns. */

#ifndef INBOX_H
#define INBOX_H

#include "json.h"

enum when { WHEN_NOW = 1, WHEN_TURN = 2, WHEN_IDLE = 4, WHEN_DEFERRED = 8 };
enum source { FROM_PROMPT, FROM_JOB, FROM_INJECT };

typedef struct inbox {
    struct item **items;
    size_t nitems, capitems, nadded, mark;
    struct output *output;
} inbox;

/* A delivery mode's name: now, turn, idle or deferred. */
const char *when_get_name(enum when when);
/* Parses a mode name, bare or as a JSON string; false if unknown. */
bool when_parse(span name_or_json_string, enum when *when);

/* Queues content from a source for a delivery mode; emits queued. */
void inbox_add(inbox *in, enum source source, enum when when, json content);
/* True if any queued item's mode is in the mask. */
bool inbox_has_due(const inbox *in, unsigned when_mask);
/* Marks now: items queued later count as queued since the mark. */
void inbox_mark(inbox *in);
/* inbox_has_due, counting only items queued since the mark. */
bool inbox_has_due_since_mark(const inbox *in, unsigned when_mask);
/* Takes the items due in the mask, and deferred ones, as blocks; a count. */
size_t inbox_take(inbox *in, unsigned when_mask, json_list *blocks);
/* Takes the prompt and inject items in the mask, as contents. */
void inbox_return_prompts(inbox *in, unsigned when_mask, json_list *contents);
/* Makes every queued job result deferred. */
void inbox_defer_jobs(inbox *in);
/* Takes every job result, whatever its mode, as blocks; a count. */
size_t inbox_take_jobs(inbox *in, json_list *blocks);
/* Counts queued items. */
size_t inbox_count(const inbox *in);

#endif
