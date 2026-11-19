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
    char       *message;
    bool        retryable;
    block_at    at;
} http_failure;

typedef http_failure (*http_failure_of)(long status, json body);

typedef struct {
    bool          (*event)(json data);
    bool          (*complete)(void);
    http_failure_of failure;
    const char     *last_event;
} http_stream_api;

/* event returns false for an event that reports an error, which then goes to failure with status 200;
   a failure whose at names a block (at.message >= 0) is reported with it.
   http_stream returns true when the stream completed and false when it was aborted. http_get returns the
   body of a 200 response. A failure that either one gives up on writes the error line and exits. */
bool http_stream(const char *url, const strv *headers, span body, const http_stream_api *api);
json http_get(const char *url, const strv *headers, http_failure_of failure);

#endif
