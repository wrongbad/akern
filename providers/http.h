/* http: calling a model API over HTTP, for the providers that do so directly. Owns libcurl, connect
   and stall timeouts, abort on SIGTERM, reading a streaming POST as server-sent events with its
   attempts and backoff (never once an event has been written), a plain GET, and failures of the
   transport itself. Knows no API: the caller parses each event's data and says what an error body
   means. */

#ifndef HTTP_H
#define HTTP_H

#include "provider.h"

typedef struct {
    const char *kind;
    char *message;
    bool retryable;
    block_at at; /* when at.message >= 0, the error names this block */
} http_failure;

typedef http_failure (*http_failure_of)(long status, json body);

typedef struct {
    bool (*event)(json data); /* false: an error event, sent to failure as status 200 */
    bool (*complete)(void);
    http_failure_of failure;
    const char *last_event;
} http_stream_api;

/* Streams a POST as SSE; false if aborted; on failure, errors and exits. */
bool http_stream(const char *url, const strv *headers, span body, const http_stream_api *api);
/* GETs a URL: the body of a 200, or the error line and exit. */
json http_get(const char *url, const strv *headers, http_failure_of failure);

#endif
