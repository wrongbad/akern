#include "http.h"
#include "media.h"
#include "provider.h"

#include <stdlib.h>
#include <string.h>

static const media_limits LIMITS = {
    .budget_bytes = 480 << 20,
    .document_budget_bytes = (50 << 20) / 3 * 4
};

typedef struct {
    long rendered[2];
    block_at canonical;
} origin;

typedef enum { ITEM_MESSAGE, ITEM_CALL, ITEM_OTHER } item_kind;

typedef struct {
    item_kind kind;
    long output_index;
    json_buf item;
    buf escaped_text;
    bool done, refused, kept;
} item;

static struct {
    origin *origins;
    size_t norigins, caporigins;
    long items_rendered, rendered_item, rendered_part;
    item *items;
    size_t nitems, capitems;
    json_buf usage, incomplete_reason;
    bool finished, incomplete;
} st;

static bool is_type(json block, const char *type)
{
    return json_eq(json_get(block, "type"), type);
}

static void put_data_url(json_buf *out, json source)
{
    buf_puts(out, "\"data:");
    buf_putspan(out, json_get_escaped_body(json_get(source, "media_type")));
    buf_puts(out, ";base64,");
    buf_putspan(out, json_get_escaped_body(json_get(source, "data")));
    buf_putc(out, '"');
}

static bool is_part(json block)
{
    return is_type(block, "text") || is_type(block, "image") || is_type(block, "document");
}

static void start_item(json_buf *out)
{
    json_put_comma_unless_first(out);
    st.rendered_item = st.items_rendered++;
    st.rendered_part = 0;
}

static void put_part(json_buf *out, json block, block_at canonical)
{
    json source = json_get(block, "source");
    if (!is_part(block))
        return;
    json_put_comma_unless_first(out);
    if (!is_type(block, "text"))
    {
        origin o = {{st.rendered_item, st.rendered_part}, canonical};
        VEC_PUSH(st.origins, st.norigins, st.caporigins, o);
    }
    st.rendered_part++;
    if (is_type(block, "text"))
    {
        json_printf(out, "{'type':'input_text','text':%R}", json_get(block, "text"));
    }
    else if (is_type(block, "image") && json_eq(json_get(source, "type"), "url"))
    {
        json_printf(out, "{'type':'input_image','image_url':%R}", json_get(source, "url"));
    }
    else if (is_type(block, "image"))
    {
        buf_puts(out, "{\"type\":\"input_image\",\"image_url\":");
        put_data_url(out, source);
        buf_putc(out, '}');
    }
    else if (is_type(block, "document"))
    {
        json name = json_get(block, "name");
        json_printf(
            out,
            "{'type':'input_file','filename':%R,'file_data':",
            name.p ? name : S("\"document\"")
        );
        put_data_url(out, source);
        buf_putc(out, '}');
    }
}

static void put_job_parts(json_buf *out, json result, block_at at)
{
    buf text = {0};
    provider_describe_job(&text, result);
    json_put_comma_unless_first(out);
    json_printf(out, "{'type':'input_text','text':%S}", buf_span(&text));
    st.rendered_part++;
    jiter it = json_iter(json_get(result, "content"));
    at.inner = 0;
    for (json block; json_next(&it, NULL, &block); at.inner++)
        if (!is_type(block, "text"))
            put_part(out, block, at);
    buf_free(&text);
}

static void put_output(json_buf *out, json content, block_at at)
{
    jiter it = json_iter(content);
    json first = NOSPAN, second;
    bool one = json_next(&it, NULL, &first) && !json_next(&it, NULL, &second);
    if (json_is(content, '"'))
        buf_putspan(out, content);
    else if (!first.p)
        buf_puts(out, "\"\"");
    else if (one && is_type(first, "text"))
        buf_putspan(out, json_get(first, "text"));
    else
    {
        buf_putc(out, '[');
        it = json_iter(content);
        at.inner = 0;
        for (json block; json_next(&it, NULL, &block); at.inner++)
            put_part(out, block, at);
        buf_putc(out, ']');
    }
}

static void put_call_answer(json_buf *out, json block, block_at at)
{
    start_item(out);
    json_printf(
        out,
        "{'type':'function_call_output','call_id':%R,'output':",
        json_get(block, "tool_use_id")
    );
    if (is_type(block, "tool_pending"))
    {
        buf text = {0};
        provider_describe_pending(&text, block);
        json_put_string(out, buf_span(&text));
        buf_free(&text);
    }
    else
    {
        put_output(out, json_get(block, "content"), at);
    }
    buf_putc(out, '}');
}

static bool answers_call(json block)
{
    return is_type(block, "tool_result") || is_type(block, "tool_pending");
}

static void put_user(json_buf *out, json message, long index)
{
    json content = json_get(message, "content");
    block_at at = {index, 0, -1};
    if (json_is(content, '"'))
    {
        start_item(out);
        json_printf(out, "{'role':'user','content':[{'type':'input_text','text':%R}]}", content);
        return;
    }
    jiter it = json_iter(content);
    bool others = false;
    for (json block; json_next(&it, NULL, &block); at.block++)
        if (answers_call(block))
            put_call_answer(out, block, at);
        else
            others = true;
    if (!others)
        return;
    start_item(out);
    buf_puts(out, "{\"role\":\"user\",\"content\":[");
    it = json_iter(content);
    at.block = 0;
    for (json block; json_next(&it, NULL, &block); at.block++)
        if (is_type(block, "job_result"))
            put_job_parts(out, block, at);
        else if (!answers_call(block))
            put_part(out, block, at);
    buf_puts(out, "]}");
}

static void put_function_call(json_buf *out, json tool_use)
{
    json input = json_get(tool_use, "input");
    json_printf(
        out,
        "{'type':'function_call','call_id':%R,'name':%R,'arguments':",
        json_get(tool_use, "id"),
        json_get(tool_use, "name")
    );
    json_put_string(out, json_is(input, '{') ? json_trim(input) : S("{}"));
    buf_putc(out, '}');
}

static void put_assistant(json_buf *out, json message)
{
    json content = json_get(message, "content");
    if (json_is(content, '"'))
    {
        start_item(out);
        json_printf(out, "{'role':'assistant','content':%R}", content);
        return;
    }
    jiter it = json_iter(content);
    for (json block; json_next(&it, NULL, &block);)
    {
        json text = json_get(block, "text"), phase = json_get(block, "x-openai-phase");
        if (is_type(block, "text") && json_get_escaped_body(text).n)
        {
            start_item(out);
            json_printf(out, "{'role':'assistant','content':%R", text);
            if (json_is(phase, '"'))
                json_printf(out, ",'phase':%R", phase);
            buf_putc(out, '}');
        }
        else if (is_type(block, "tool_use"))
        {
            start_item(out);
            put_function_call(out, block);
        }
        else if (is_type(block, "x-openai"))
        {
            start_item(out);
            buf_putspan(out, json_get(block, "item"));
        }
    }
}

static void put_input(json_buf *out, json messages)
{
    json_buf limited = {0};
    media_put_within_limits(&limited, messages, &LIMITS);
    jiter it = json_iter(buf_span(&limited));
    buf_putc(out, '[');
    long index = 0;
    for (json message; json_next(&it, NULL, &message); index++)
        if (json_eq(json_get(message, "role"), "user"))
            put_user(out, message, index);
        else
            put_assistant(out, message);
    buf_putc(out, ']');
    buf_free(&limited);
}

static void put_tools(json_buf *out, json tools)
{
    jiter it = json_iter(tools);
    buf_puts(out, ",\"tools\":[");
    for (json tool; json_next(&it, NULL, &tool);)
    {
        json_put_comma_unless_first(out);
        json_printf(
            out,
            "{'type':'function','name':%R,'description':%R,'parameters':%R,'strict':false}",
            json_get(tool, "name"),
            json_get(tool, "description"),
            json_get(tool, "input_schema")
        );
    }
    buf_putc(out, ']');
}

static void put_reasoning(json_buf *out, json params)
{
    json effort = json_get(params, "effort"), summary = json_get(params, "summary");
    json_printf(out, ",'reasoning':{'effort':%R", json_is(effort, '"') ? effort : S("\"high\""));
    if (!json_eq(summary, "off"))
        json_printf(out, ",'summary':%R", json_is(summary, '"') ? summary : S("\"auto\""));
    buf_putc(out, '}');
}

static void put_max_output_tokens(json_buf *out, json params)
{
    long long n = (long long)json_get_num(json_get(params, "max_tokens"), 0);
    if (n >= 1)
        json_printf(out, ",'max_output_tokens':%D", n);
}

static void build_request(json_buf *b, json request)
{
    json model_id = json_get(request, "model"), system = json_get(request, "system");
    json tools = json_get(request, "tools"), params = json_get(request, "params");
    json_printf(
        b,
        "{'model':%R,'stream':true,'store':false,'include':['reasoning.encrypted_content']",
        model_id
    );
    if (json_is(system, '"') && !json_eq(system, ""))
        json_printf(b, ",'instructions':%R", system);
    buf_puts(b, ",\"input\":");
    put_input(b, json_get(request, "messages"));
    if (json_is(tools, '[') && tools.n > 2)
        put_tools(b, tools);
    put_reasoning(b, params);
    put_max_output_tokens(b, params);
    buf_putc(b, '}');
}

static void url(buf *u, const char *path)
{
    const char *base = getenv("OPENAI_BASE_URL");
    buf_puts(u, base && *base ? base : "https://api.openai.com/v1");
    while (u->n && u->p[u->n - 1] == '/')
        u->n--;
    buf_puts(u, path);
}

static void headers(strv *h)
{
    buf auth = {0};
    buf_printf(&auth, "authorization: Bearer %s", getenv("OPENAI_API_KEY"));
    strv_push(h, xstrdup("content-type: application/json"));
    strv_push(h, xstrdup(buf_cstr(&auth)));
    buf_free(&auth);
}

static void start_response(void)
{
    for (size_t i = 0; i < st.nitems; i++)
    {
        buf_free(&st.items[i].item);
        buf_free(&st.items[i].escaped_text);
    }
    st.nitems = 0;
    st.usage.n = st.incomplete_reason.n = 0;
    st.finished = st.incomplete = false;
}

static item *item_at(json event)
{
    long index = (long)json_get_num(json_get(event, "output_index"), -1);
    for (size_t i = st.nitems; i-- > 0;)
        if (st.items[i].output_index == index)
            return &st.items[i];
    return NULL;
}

static item_kind kind_of(json type)
{
    if (json_eq(type, "message"))
        return ITEM_MESSAGE;
    if (json_eq(type, "function_call"))
        return ITEM_CALL;
    return ITEM_OTHER;
}

static item *add_item(json event)
{
    json added = json_get(event, "item");
    item it = {
        .kind = kind_of(json_get(added, "type")),
        .output_index = (long)json_get_num(json_get(event, "output_index"), -1)
    };
    buf_set(&it.item, json_trim(added));
    VEC_PUSH(st.items, st.nitems, st.capitems, it);
    if (it.kind == ITEM_CALL)
        provider_emit(
            "{'type':'tool_use_start','id':%R,'name':%R}",
            json_get(added, "call_id"),
            json_get(added, "name")
        );
    return &st.items[st.nitems - 1];
}

static void finish_item(json event)
{
    item *it = item_at(event);
    if (!it)
        it = add_item(event);
    buf_set(&it->item, json_trim(json_get(event, "item")));
    it->done = true;
}

static void add_text(json event, bool refusal)
{
    item *it = item_at(event);
    json delta = json_get(event, "delta");
    if (!it)
        return;
    buf_putspan(&it->escaped_text, json_get_escaped_body(delta));
    it->refused |= refusal;
    provider_emit("{'type':'text_delta','text':%R}", delta);
}

static void add_arguments(json event)
{
    item *it = item_at(event);
    json delta = json_get(event, "delta");
    if (!it)
        return;
    provider_emit(
        "{'type':'tool_use_delta','id':%R,'json':%R}",
        json_get(buf_span(&it->item), "call_id"),
        delta
    );
}

static void add_thinking(json event)
{
    json delta = json_get(event, "delta");
    if (json_get_escaped_body(delta).n)
        provider_emit("{'type':'thinking_delta','text':%R}", delta);
}

static void finish_response(json response, bool incomplete)
{
    buf_set(&st.usage, json_trim(json_get(response, "usage")));
    buf_set(
        &st.incomplete_reason,
        json_trim(json_get(json_get(response, "incomplete_details"), "reason"))
    );
    st.incomplete = incomplete;
    st.finished = true;
}

static bool on_event(json event)
{
    json type = json_get(event, "type");
    if (json_eq(type, "response.created"))
        start_response();
    else if (json_eq(type, "response.output_item.added"))
        add_item(event);
    else if (json_eq(type, "response.output_item.done"))
        finish_item(event);
    else if (json_eq(type, "response.output_text.delta"))
        add_text(event, false);
    else if (json_eq(type, "response.refusal.delta"))
        add_text(event, true);
    else if (json_eq(type, "response.function_call_arguments.delta"))
        add_arguments(event);
    else if (
        json_eq(type, "response.reasoning_summary_text.delta") ||
        json_eq(type, "response.reasoning_text.delta")
    )
        add_thinking(event);
    else if (
        json_eq(type, "response.reasoning_summary_part.added") &&
        json_get_num(json_get(event, "summary_index"), 0) > 0
    )
        provider_emit("{'type':'thinking_delta','text':'\\n\\n'}");
    else if (json_eq(type, "response.completed"))
        finish_response(json_get(event, "response"), false);
    else if (json_eq(type, "response.incomplete"))
        finish_response(json_get(event, "response"), true);
    return !json_eq(type, "error") && !json_eq(type, "response.failed");
}

static bool finished(void)
{
    return st.finished;
}

static bool any_call(void)
{
    for (size_t i = 0; i < st.nitems; i++)
        if (st.items[i].kind == ITEM_CALL && st.items[i].done)
            return true;
    return false;
}

static bool any_refusal(void)
{
    for (size_t i = 0; i < st.nitems; i++)
    {
        jiter it = json_iter(json_get(buf_span(&st.items[i].item), "content"));
        for (json part; json_next(&it, NULL, &part);)
            if (is_type(part, "refusal"))
                return true;
        if (st.items[i].refused)
            return true;
    }
    return false;
}

static const char *stop_name(bool aborted)
{
    json reason = buf_span(&st.incomplete_reason);
    if (aborted)
        return "aborted";
    if (st.incomplete && json_eq(reason, "max_output_tokens"))
        return "max_tokens";
    if (st.incomplete && json_eq(reason, "content_filter"))
        return "refusal";
    if (!st.incomplete && any_call())
        return "tool_use";
    if (any_refusal())
        return "refusal";
    return "end_turn";
}

static void mark_kept(bool aborted, bool tool_use)
{
    bool later = false;
    for (size_t i = st.nitems; i-- > 0;)
    {
        item *it = &st.items[i];
        if (it->kind == ITEM_MESSAGE)
            it->kept = it->done || it->escaped_text.n;
        else if (it->kind == ITEM_CALL)
            it->kept = !aborted && tool_use && it->done;
        else
            it->kept = !aborted && it->done && later;
        later |= it->kept;
    }
}

static void put_text(json_buf *out, json text, json phase)
{
    json_put_comma_unless_first(out);
    json_printf(out, "{'type':'text','text':%R", text);
    if (json_is(phase, '"'))
        json_printf(out, ",'x-openai-phase':%R", phase);
    buf_putc(out, '}');
}

static void put_message(json_buf *out, item *it)
{
    json message = buf_span(&it->item), phase = json_get(message, "phase");
    if (!it->done)
    {
        json_buf text = {0};
        json_printf(&text, "'%r'", buf_cstr(&it->escaped_text));
        put_text(out, buf_span(&text), phase);
        buf_free(&text);
        return;
    }
    jiter parts = json_iter(json_get(message, "content"));
    for (json part; json_next(&parts, NULL, &part);)
    {
        json text = json_get(part, is_type(part, "refusal") ? "refusal" : "text");
        if (json_get_escaped_body(text).n)
            put_text(out, text, phase);
    }
}

static void put_tool_input(json_buf *out, json arguments)
{
    buf decoded = {0};
    json_decode(arguments, &decoded);
    json input = json_trim(buf_span(&decoded));
    if (!input.n)
        buf_puts(out, "{}");
    else if (json_is(input, '{') && json_is_valid(input))
        buf_putspan(out, input);
    else
        json_put_string(out, input);
    buf_free(&decoded);
}

static void put_tool_use(json_buf *out, const item *it)
{
    json call = buf_span(&it->item);
    json_put_comma_unless_first(out);
    json_printf(
        out,
        "{'type':'tool_use','id':%R,'name':%R,'input':",
        json_get(call, "call_id"),
        json_get(call, "name")
    );
    put_tool_input(out, json_get(call, "arguments"));
    buf_putc(out, '}');
}

static void put_content(json_buf *out, bool aborted, bool tool_use)
{
    mark_kept(aborted, tool_use);
    buf_putc(out, '[');
    for (size_t i = 0; i < st.nitems; i++)
    {
        item *it = &st.items[i];
        if (!it->kept)
            continue;
        if (it->kind == ITEM_MESSAGE)
            put_message(out, it);
        else if (it->kind == ITEM_CALL)
            put_tool_use(out, it);
        else
            json_put_comma_unless_first(out),
                json_printf(out, "{'type':'x-openai','item':%R}", buf_span(&it->item));
    }
    buf_putc(out, ']');
}

static long long tokens(json usage, const char *details, const char *key)
{
    json v = json_get(details ? json_get(usage, details) : usage, key);
    return (long long)json_get_num(v, 0);
}

static void result(bool aborted)
{
    json usage = buf_span(&st.usage);
    const char *stop = stop_name(aborted);
    json_buf content = {0};
    put_content(&content, aborted, !strcmp(stop, "tool_use"));
    long long cache_read = tokens(usage, "input_tokens_details", "cached_tokens");
    long long cache_write = tokens(usage, "input_tokens_details", "cache_write_tokens");
    long long input = tokens(usage, NULL, "input_tokens") - cache_read - cache_write;
    provider_emit_result(
        stop,
        buf_span(&content),
        (
            provider_usage
        ){input > 0 ? input : 0, tokens(usage, NULL, "output_tokens"), cache_read, cache_write}
    );
}

static bool mentions_overflow(const char *message)
{
    return strstr(message, "context window") || strstr(message, "context length") ||
           strstr(message, "maximum context");
}

static bool out_of_quota(json code, json type)
{
    return json_eq(type, "insufficient_quota") || json_eq(code, "insufficient_quota") ||
           json_eq(code, "credit_balance_exhausted") ||
           json_eq(code, "organization_spend_limit_exceeded") ||
           json_eq(code, "project_spend_limit_exceeded") ||
           json_eq(code, "organization_usage_limit_exceeded");
}

static const char *error_kind(long s, json code, json type, const char *message)
{
    if (s == 401 || s == 403 || json_eq(code, "invalid_api_key"))
        return "auth";
    if (out_of_quota(code, type))
        return "other";
    if (s == 413 || json_eq(code, "context_length_exceeded") || mentions_overflow(message))
        return "context_overflow";
    if (s == 429 || json_eq(code, "rate_limit_exceeded") || json_eq(type, "rate_limit_error"))
        return "rate_limit";
    bool overloaded =
        json_eq(code, "server_is_overloaded") || json_eq(type, "service_unavailable_error");
    if (s == 503 || overloaded)
        return "overloaded";
    if (s >= 400 && s < 500)
        return "invalid_request";
    return "other";
}

static json error_of(json body)
{
    json error = json_get(body, "error"), response = json_get(body, "response");
    if (json_is(error, '{'))
        return error;
    if (json_is(response, '{'))
        return json_get(response, "error");
    return body;
}

static bool read_index(const char **p, const char *before, long *index)
{
    char *end;
    size_t n = strlen(before);
    if (strncmp(*p, before, n) || (*p)[n] < '0' || (*p)[n] > '9')
        return false;
    *index = strtol(*p + n, &end, 10);
    *p = end;
    return **p == ']';
}

static block_at blamed_block(json param)
{
    buf text = {0};
    long rendered[2];
    const char *p = json_decode(param, &text) ? buf_cstr(&text) : "";
    bool named =
        read_index(&p, "input[", &rendered[0]) &&
        (read_index(&p, "].content[", &rendered[1]) || read_index(&p, "].output[", &rendered[1]));
    block_at at = NO_BLOCK;
    for (size_t i = 0; named && i < st.norigins; i++)
        if (!memcmp(st.origins[i].rendered, rendered, sizeof rendered))
            at = st.origins[i].canonical;
    buf_free(&text);
    return at;
}

static http_failure api_failure(long status, json body)
{
    json error = error_of(body);
    json code = json_get(error, "code"), type = json_get(error, "type"),
         message = json_get(error, "message");
    char *text =
        json_is(message, '"') ? json_decode_dup(message) : xstrdup("unexpected error response");
    const char *kind = error_kind(status, code, type, text);
    bool retryable = !strcmp(kind, "rate_limit") || !strcmp(kind, "overloaded") ||
                     (!strcmp(kind, "other") && !out_of_quota(code, type) &&
                         (status >= 500 || json_eq(code, "server_error")));
    block_at at =
        strcmp(kind, "invalid_request") ? NO_BLOCK : blamed_block(json_get(error, "param"));
    return (http_failure){at.message >= 0 ? "invalid_block" : kind, text, retryable, at};
}

static const http_stream_api RESPONSES_API =
    {on_event, finished, api_failure, "response.completed"};

static void require_key(void)
{
    if (!getenv("OPENAI_API_KEY"))
        provider_emit_error("auth", "OPENAI_API_KEY is not set", 0);
}

static int list_models(void)
{
    buf u = {0};
    strv h = {0};
    require_key();
    url(&u, "/models");
    headers(&h);
    jiter it = json_iter(json_get(http_get(buf_cstr(&u), &h, api_failure), "data"));
    for (json listed; json_next(&it, NULL, &listed);)
    {
        if (!json_is(json_get(listed, "id"), '"'))
            continue;
        char *id = json_decode_dup(json_get(listed, "id"));
        provider_emit_model(id, 0, 0);
        free(id);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--models"))
        return list_models();
    json request = provider_read_request();
    json_buf body = {0};
    build_request(&body, request);
    require_key();
    buf u = {0};
    strv h = {0};
    url(&u, "/responses");
    headers(&h);
    result(!http_stream(buf_cstr(&u), &h, buf_span(&body), &RESPONSES_API));
}
