/* provider: the provider side of the kernel's provider protocol (docs/providers.md), shared by
   every provider. Owns reading the request, writing event, result and error lines with the exit
   status each implies, SIGTERM as abort, and how the facts no API has a block for are worded:
   calls still running and late job results. Knows no model API. */

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

/* Traps SIGTERM and reads the request, one line of stdin; exits if it is missing or invalid. */
json provider_read_request(void);
/* For a provider that stays: traps SIGTERM and reads the next request line. False at EOF or
   once SIGTERM has arrived; exits if the line is invalid. */
bool provider_next_request(json *request);
/* From now on a result line says the provider stays for the next request, and does not exit. */
void provider_stay(void);
/* True once SIGTERM has arrived. */
bool provider_was_aborted(void);

/* Writes one event line, formatted as by json_printf. */
void provider_emit(const char *fmt, ...);
/* True once any event line has been written for the current request. */
bool provider_has_emitted(void);
/* Writes one model line; a limit of 0 is left out. */
void provider_emit_model(const char *id, long long context, long long max_output);
/* Writes the result line and exits, or, after provider_stay, returns. */
void provider_emit_result(const char *stop, json content, provider_usage usage);
/* Writes the error line and exits. */
void provider_emit_error(const char *kind, const char *message, long status);
/* provider_emit_error, naming the block the API rejected. */
void provider_emit_error_at(const char *kind, const char *message, long status, block_at at);

/* The calls of the latest assistant message that no user message has answered yet. Every call
   must be answered in the user message right after its assistant message, so a call still
   running then gets a placeholder from the provider, and its real result, which arrives in a
   later message, is worded as text (providers.md §5). */
typedef struct {
    json *uses; /* tool_use blocks */
    size_t n, cap;
} open_calls;

/* Makes the assistant message's tool_use blocks the open calls. */
void open_calls_set(open_calls *oc, json assistant_message);
/* True, and the call no longer open, when tool_use_id names an open call. */
bool open_calls_take(open_calls *oc, json tool_use_id);
/* True when tool_use_id is in ids, a list of JSON strings. */
bool ids_contain(const json *ids, size_t n, json tool_use_id);

/* Words the placeholder answer of a call still running, from its tool_use block. */
void provider_describe_unanswered(buf *text, json tool_use);
/* Words a tool_pending block (sessions from protocol 1) as text for the model. */
void provider_describe_pending(buf *text, json pending);
/* Words a late result as text for the model: a tool_result delivered after its turn, or a
   job_result (sessions from protocol 1). */
void provider_describe_job(buf *text, json job_result);

#endif
