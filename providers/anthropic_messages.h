/* anthropic_messages: the Anthropic Messages format, for the providers whose model speaks it. Owns
   rendering the canonical messages as a Messages request's messages (call answers first, adjacent
   user messages merged, calls still running answered with placeholders and late results worded
   as text, a non-object tool input as {},
   x-* blocks and fields dropped, media within the API's size limits), finding the canonical block
   an API error names, and reading a Messages stream into events and a result. Knows no
   transport: the caller feeds it the stream's events. */

#ifndef ANTHROPIC_MESSAGES_H
#define ANTHROPIC_MESSAGES_H

#include "provider.h"

/* Sets the prefix API tool names carry; "" by default. */
void anthropic_name_tools(const char *prefix);
/* Renders canonical messages as a Messages request's messages. */
void anthropic_render(json_buf *out, json messages);

/* The rendered block an API error message names, and its reason. */
bool anthropic_blame(const char *api_message, block_at *at, const char **reason);

/* Reads one stream event, emitting events; false for an error event. */
bool anthropic_read_event(json event);
/* True once the stream has ended with message_stop. */
bool anthropic_is_complete(void);
/* Writes the result line from what the stream gave, and exits. */
void anthropic_emit_result(bool aborted);

#endif
