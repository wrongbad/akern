/* provider: the provider side of the kernel's provider protocol (docs/providers.md), shared by
   every provider. Owns reading the request, writing event, result and error lines with the exit
   status each implies, SIGTERM as abort, and how the facts no API has a block for are worded:
   calls still pending and late job results. Knows no model API. */

#ifndef PROVIDER_H
#define PROVIDER_H

#include "json.h"

typedef struct { long long input, output, cache_read, cache_write; } provider_usage;

/* A block of the request's messages: content index block of message, and inner, the index inside a
   tool_result's or job_result's content; -1 when absent. */
typedef struct { long message, block, inner; } block_at;

#define NO_BLOCK ((block_at){ -1, -1, -1 })

/* provider_request traps SIGTERM and reads stdin; provider_result, provider_error(_at) and an invalid
   request write the terminal line and exit; provider_error_at adds where the API rejected a block. provider_model leaves out a limit that is 0. */
json provider_request(void);
bool provider_aborted(void);

void provider_emit(const char *fmt, ...);
bool provider_emitted(void);
void provider_model(const char *id, long long context, long long max_output);
void provider_result(const char *stop, json content, provider_usage usage);
void provider_error(const char *kind, const char *message, long status);
void provider_error_at(const char *kind, const char *message, long status, block_at at);

void provider_describe_pending(buf *text, json pending);
void provider_describe_job(buf *text, json job_result);

#endif
