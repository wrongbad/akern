/* msg: the shape of conversation content (protocols.md §1). Owns building content blocks
   (text, media, tool results) and user messages, and the few things the kernel needs to know
   about a message: whether it is empty, its text, and what remains without its tool_use
   blocks. Every other part of a message is opaque. Storing messages is session's; deciding
   what goes in them is the driver's and calls'. */

#ifndef MSG_H
#define MSG_H

#include "json.h"

void blocks_add_content(json_list *blocks, json content);
void blocks_add_text(json_list *blocks, span text);
void blocks_add_tool_result(json_list *blocks, const char *tool_use_id, bool is_error, json content);
void blocks_add_media(json_list *blocks, const char *media_type, span bytes);
void content_from_text(json_buf *content, span text);
void content_split_text(json content, buf *text, json_list *other_blocks);
bool content_valid(json content);
void message_user(json_buf *message, const json_list *blocks);
bool message_strip_tool_use(json message, json_buf *stripped);
bool message_empty(json message);

#endif
