#include "message.h"

#include <string.h>

static void comma(json_list *list)
{
    if (list->n)
        buf_putc(list, ',');
}

void blocks_add_text(json_list *blocks, span text)
{
    comma(blocks);
    json_printf(blocks, "{'type':'text','text':%S}", text.n ? text : S("(empty)"));
}

void blocks_add_content(json_list *blocks, json content)
{
    content = json_trim(content);
    if (json_is(content, '"'))
    {
        buf text = {0};
        json_decode(content, &text);
        blocks_add_text(blocks, buf_span(&text));
        buf_free(&text);
        return;
    }
    jiter it = json_iter(content);
    for (json block; json_next(&it, NULL, &block);)
    {
        comma(blocks);
        buf_putspan(blocks, block);
    }
}

static void put_base64(buf *out, span bytes)
{
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *p = (const unsigned char *)bytes.p;
    for (size_t i = 0; i < bytes.n; i += 3)
    {
        unsigned long v = (unsigned long)p[i] << 16;
        if (i + 1 < bytes.n)
            v |= (unsigned long)p[i + 1] << 8;
        if (i + 2 < bytes.n)
            v |= p[i + 2];
        char quad[4] = {
            digits[v >> 18 & 63],
            digits[v >> 12 & 63],
            i + 1 < bytes.n ? digits[v >> 6 & 63] : '=',
            i + 2 < bytes.n ? digits[v & 63] : '='
        };
        buf_put(out, quad, 4);
    }
}

void blocks_add_media(json_list *blocks, const char *media_type, const char *name, span bytes)
{
    bool image = strncmp(media_type, "image/", 6) == 0;
    comma(blocks);
    json_printf(blocks, "{'type':%s,", image ? "image" : "document");
    if (name && !image)
        json_printf(blocks, "'name':%s,", name);
    json_printf(blocks, "'source':{'type':'base64','media_type':%s,'data':'", media_type);
    put_base64(blocks, bytes);
    buf_puts(blocks, "\"}}");
}

void blocks_add_tool_result(json_list *blocks, const char *tool_use_id, bool is_error, json content)
{
    comma(blocks);
    json_printf(
        blocks,
        "{'type':'tool_result','tool_use_id':%s,'is_error':%b,'content':%R}",
        tool_use_id,
        is_error,
        content
    );
}

void content_put_text(json_buf *content, span text)
{
    json_list blocks = {0};
    blocks_add_text(&blocks, text);
    json_printf(content, "[%R]", buf_span(&blocks));
    buf_free(&blocks);
}

void content_split_text(json content, buf *text, json_list *other_blocks)
{
    if (json_is(content, '"'))
    {
        json_decode(content, text);
        return;
    }
    jiter it = json_iter(content);
    for (json block; json_next(&it, NULL, &block);)
    {
        if (json_eq(json_get(block, "type"), "text"))
            json_decode(json_get(block, "text"), text);
        else
            comma(other_blocks), buf_putspan(other_blocks, block);
    }
}

bool content_is_valid(json content)
{
    content = json_trim(content);
    return json_is_valid(content) && (json_is(content, '"') || json_is(content, '['));
}

void message_put_user(json_buf *message, const json_list *blocks)
{
    json_printf(message, "{'role':'user','content':[");
    buf_putspan(message, buf_span(blocks));
    buf_puts(message, "]}");
}

bool message_strip_tool_use(json message, json_buf *stripped)
{
    json_list blocks = {0};
    jiter it = json_iter(json_get(message, "content"));
    for (json block; json_next(&it, NULL, &block);)
        if (!json_eq(json_get(block, "type"), "tool_use"))
            comma(&blocks), buf_putspan(&blocks, block);
    bool any = blocks.n > 0;
    if (any)
        json_printf(stripped, "{'role':'assistant','content':[%r]}", buf_cstr(&blocks));
    buf_free(&blocks);
    return any;
}

bool message_is_empty(json message)
{
    json block;
    jiter it = json_iter(json_get(message, "content"));
    return !json_next(&it, NULL, &block);
}

static bool put_without_media(json_buf *out, json object, const long *path, span reason);

static bool put_note(json_buf *out, json block, span reason)
{
    json type = json_get(block, "type");
    bool image = json_eq(type, "image");
    if (!image && !json_eq(type, "document"))
        return false;
    buf note = {0};
    buf_printf(&note, "[%s omitted: %.*s]", image ? "image" : "document", (int)reason.n, reason.p);
    json_printf(out, "{'type':'text','text':%S}", buf_span(&note));
    buf_free(&note);
    return true;
}

static bool put_content_without_media(json_buf *out, json content, const long *path, span reason)
{
    jiter it = json_iter(content);
    bool omitted = false;
    buf_putc(out, '[');
    for (long i = 0; json_next(&it, NULL, &content); i++)
    {
        size_t at = out->n + (i > 0);
        if (i)
            buf_putc(out, ',');
        bool here =
            i == path[0] && (path[1] < 0 ? put_note(out, content, reason)
                                         : put_without_media(out, content, path + 1, reason));
        if (!here)
            out->n = at, buf_putspan(out, content);
        omitted |= here;
    }
    buf_putc(out, ']');
    return omitted;
}

static bool put_without_media(json_buf *out, json object, const long *path, span reason)
{
    jiter it = json_iter(object);
    bool omitted = false, first = true;
    buf_putc(out, '{');
    for (json k, v; json_next(&it, &k, &v); first = false)
    {
        json_printf(out, "%r%R:", first ? "" : ",", k);
        if (json_eq(k, "content") && json_is(v, '['))
            omitted |= put_content_without_media(out, v, path, reason);
        else
            buf_putspan(out, v);
    }
    buf_putc(out, '}');
    return omitted;
}

bool message_omit_media(json message, long block, long inner, span reason, json_buf *out)
{
    long path[] = {block, inner, -1};
    return block >= 0 && put_without_media(out, message, path, reason);
}
