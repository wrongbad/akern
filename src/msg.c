#include "msg.h"

#include <string.h>

static void comma(json_list *list)
{
    if (list->n) buf_putc(list, ',');
}

void blocks_add_text(json_list *blocks, span text)
{
    comma(blocks);
    json_printf(blocks, "{'type':'text','text':%S}", text.n ? text : S("(empty)"));
}

void blocks_add_content(json_list *blocks, json content)
{
    content = json_trim(content);
    if (json_is(content, '"')) {
        buf text = { 0 };
        json_text(content, &text);
        blocks_add_text(blocks, buf_span(&text));
        buf_free(&text);
        return;
    }
    jiter it = json_iter(content);
    for (json block; json_next(&it, NULL, &block);) {
        comma(blocks);
        buf_putspan(blocks, block);
    }
}

static void put_base64(buf *out, span bytes)
{
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *p = (const unsigned char *)bytes.p;
    for (size_t i = 0; i < bytes.n; i += 3) {
        unsigned long v = (unsigned long)p[i] << 16;
        if (i + 1 < bytes.n) v |= (unsigned long)p[i + 1] << 8;
        if (i + 2 < bytes.n) v |= p[i + 2];
        char quad[4] = { digits[v >> 18 & 63], digits[v >> 12 & 63],
                         i + 1 < bytes.n ? digits[v >> 6 & 63] : '=', i + 2 < bytes.n ? digits[v & 63] : '=' };
        buf_put(out, quad, 4);
    }
}

void blocks_add_media(json_list *blocks, const char *media_type, span bytes)
{
    comma(blocks);
    json_printf(blocks, "{'type':%s,'source':{'type':'base64','media_type':%s,'data':'",
                strncmp(media_type, "image/", 6) == 0 ? "image" : "document", media_type);
    put_base64(blocks, bytes);
    buf_puts(blocks, "\"}}");
}

void blocks_add_tool_result(json_list *blocks, const char *tool_use_id, bool is_error, json content)
{
    comma(blocks);
    json_printf(blocks, "{'type':'tool_result','tool_use_id':%s,'is_error':%b,'content':%R}",
                tool_use_id, is_error, content);
}

void content_from_text(json_buf *content, span text)
{
    json_list blocks = { 0 };
    blocks_add_text(&blocks, text);
    json_printf(content, "[%R]", buf_span(&blocks));
    buf_free(&blocks);
}

void content_split_text(json content, buf *text, json_list *other_blocks)
{
    if (json_is(content, '"')) {
        json_text(content, text);
        return;
    }
    jiter it = json_iter(content);
    for (json block; json_next(&it, NULL, &block);) {
        if (json_eq(json_get(block, "type"), "text")) json_text(json_get(block, "text"), text);
        else comma(other_blocks), buf_putspan(other_blocks, block);
    }
}

bool content_valid(json content)
{
    content = json_trim(content);
    return json_valid(content) && (json_is(content, '"') || json_is(content, '['));
}

void message_user(json_buf *message, const json_list *blocks)
{
    json_printf(message, "{'role':'user','content':[");
    buf_putspan(message, buf_span(blocks));
    buf_puts(message, "]}");
}

bool message_strip_tool_use(json message, json_buf *stripped)
{
    json_list blocks = { 0 };
    jiter it = json_iter(json_get(message, "content"));
    for (json block; json_next(&it, NULL, &block);)
        if (!json_eq(json_get(block, "type"), "tool_use")) comma(&blocks), buf_putspan(&blocks, block);
    bool any = blocks.n > 0;
    if (any) json_printf(stripped, "{'role':'assistant','content':[%r]}", buf_cstr(&blocks));
    buf_free(&blocks);
    return any;
}

bool message_empty(json message)
{
    json block;
    jiter it = json_iter(json_get(message, "content"));
    return !json_next(&it, NULL, &block);
}
