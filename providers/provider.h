/* provider: the provider side of the kernel's provider protocol (docs/providers.md), shared by
   every provider. Owns reading the request, writing event, result and error lines with the exit
   status each implies, SIGTERM as abort, and how the facts no API has a block for are worded:
   calls still pending and late job results. Knows no model API. */

#ifndef PROVIDER_H
#define PROVIDER_H

#include "json.h"

typedef struct {
    long long input, output, cache_read, cache_write;
} provider_usage;

/* A block of the request: the message's index, the block's index in its content, and
   inner, the index inside a tool_result's or job_result's content; -1 when absent. */
typedef struct {
    long message, block, inner;
} block_at;

#define NO_BLOCK ((block_at){-1, -1, -1})

/* Traps SIGTERM and reads the request from stdin; exits if invalid. */
json provider_read_request(void);
/* True once SIGTERM has arrived. */
bool provider_was_aborted(void);

/* Writes one event line, formatted as by json_printf. */
void provider_emit(const char *fmt, ...);
/* True once any event line has been written. */
bool provider_has_emitted(void);
/* Writes one model line; a limit of 0 is left out. */
void provider_emit_model(const char *id, long long context, long long max_output);
/* Writes the result line and exits. */
void provider_emit_result(const char *stop, json content, provider_usage usage);
/* Writes the error line and exits. */
void provider_emit_error(const char *kind, const char *message, long status);
/* provider_emit_error, naming the block the API rejected. */
void provider_emit_error_at(const char *kind, const char *message, long status, block_at at);

/* Words a tool_pending block as text for the model. */
void provider_describe_pending(buf *text, json pending);
/* Words a job_result block as text for the model. */
void provider_describe_job(buf *text, json job_result);

#endif
