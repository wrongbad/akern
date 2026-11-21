/* message: the shape of conversation content (providers.md §5). Owns building content blocks
   (text, media, tool results) and user messages, and the few things the kernel needs to know
   about a message: whether it is empty, its text, what remains without its tool_use
   blocks, and the message with one image or document block swapped for a note. Every other part of
   a message is opaque. Storing messages is session's; deciding
   what goes in them is turns' and calls'. */

#ifndef MESSAGE_H
#define MESSAGE_H

#include "json.h"

/* Adds content: a string as a text block, an array's blocks as is. */
void blocks_add_content(json_list *blocks, json content);
/* Adds a text block; empty text becomes (empty). */
void blocks_add_text(json_list *blocks, span text);
/* Adds a tool_result block. */
void blocks_add_tool_result(
    json_list *blocks,
    const char *tool_use_id,
    bool is_error,
    json content
);
/* Adds a base64 image block for an image type, else a named document block. */
void blocks_add_media(json_list *blocks, const char *media_type, const char *name, span bytes);
/* Writes a content array of one text block. */
void content_put_text(json_buf *content, span text);
/* Splits content into its joined text and its other blocks. */
void content_split_text(json content, buf *text, json_list *other_blocks);
/* True for valid JSON that is a string or an array. */
bool content_is_valid(json content);
/* Writes a user message holding these blocks. */
void message_put_user(json_buf *message, const json_list *blocks);
/* Writes the message without tool_use blocks; false if none remain. */
bool message_strip_tool_use(json message, json_buf *stripped);
/* True if the message has no content blocks. */
bool message_is_empty(json message);
/* Writes the message with one image or document swapped for a note. */
bool message_omit_media(json message, long block, long inner, span reason, json_buf *out);

#endif
