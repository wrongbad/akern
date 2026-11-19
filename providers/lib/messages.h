/* messages: the Anthropic Messages format, for the providers whose model speaks it. Owns rendering
   the canonical messages as a Messages request's messages (call answers first, adjacent user
   messages merged, tool_pending and job_result worded as text, a non-object tool input as {},
   x-* blocks and fields dropped, media within the API's size limits), finding the canonical block
   an API error names, and reading a Messages stream into events and a result. Knows no
   transport: the caller feeds it the stream's events. */

#ifndef MESSAGES_H
#define MESSAGES_H

#include "provider.h"

/* Tool names in the API carry this prefix: added when rendering, removed when reading. Default "". */
void messages_name_tools(const char *prefix);
void messages_render(json_buf *out, json messages);

/* messages_blame: the media block of the last rendered request that an API error message names
   ("messages.3.content.1…: reason"), and the reason after the position. */
bool messages_blame(const char *api_message, block_at *at, const char **reason);

/* messages_event returns false for an error event; messages_result writes the result line and exits. */
bool messages_event(json event);
bool messages_complete(void);
void messages_result(bool aborted);

#endif
