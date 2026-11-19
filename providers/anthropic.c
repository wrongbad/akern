#include "lib/messages.h"
#include "lib/provider.h"
#include "lib/http.h"

#include <stdlib.h>
#include <string.h>

enum { DEFAULT_MAX_TOKENS = 64000 };

static long long max_tokens(json params)
{
    long long n = (long long)json_num(json_get(params, "max_tokens"), DEFAULT_MAX_TOKENS);
    return n < 1 ? DEFAULT_MAX_TOKENS : n;
}

static bool off(json params, const char *key)
{
    return json_eq(json_get(params, key), "off");
}

static json effort(json params)
{
    json e = json_get(params, "effort");
    return json_is(e, '"') ? e : S("\"high\"");
}

static void put_tool(json_buf *b, json tool)
{
    json_printf(b, "{'name':%R,'description':%R,'input_schema':%R,'eager_input_streaming':true}",
                json_get(tool, "name"), json_get(tool, "description"), json_get(tool, "input_schema"));
}

static void put_tools(json_buf *b, json tools)
{
    jiter it = json_iter(tools);
    json tool;
    buf_puts(b, ",\"tools\":[");
    for (int i = 0; json_next(&it, NULL, &tool); i++) {
        if (i) buf_putc(b, ',');
        put_tool(b, tool);
    }
    buf_putc(b, ']');
}

static void put_thinking(json_buf *b, json params)
{
    json thinking = json_get(params, "thinking");
    if (json_is(thinking, '{')) json_printf(b, ",'thinking':%R", thinking);
    else if (!off(params, "thinking")) json_printf(b, ",'thinking':{'type':'adaptive','display':'summarized'}");
    if (!off(params, "effort")) json_printf(b, ",'output_config':{'effort':%R}", effort(params));
}

static void build_request(json_buf *b, json kernel_request, bool fallbacks)
{
    json model_id = json_get(kernel_request, "model"), system = json_get(kernel_request, "system");
    json tools = json_get(kernel_request, "tools"), params = json_get(kernel_request, "params");
    json_printf(b, "{'model':%R,'max_tokens':%D,'stream':true", model_id, max_tokens(params));
    if (!json_null_or_absent(system) && !json_eq(system, "")) json_printf(b, ",'system':%R", system);
    buf_puts(b, ",\"messages\":");
    messages_render(b, json_get(kernel_request, "messages"));
    if (json_is(tools, '[') && tools.n > 2) put_tools(b, tools);
    put_thinking(b, params);
    if (fallbacks) json_printf(b, ",'fallbacks':'default'");
    json_printf(b, ",'cache_control':{'type':'ephemeral'}}");
}

static void url(buf *u, const char *path)
{
    const char *base = getenv("ANTHROPIC_BASE_URL");
    buf_puts(u, base && *base ? base : "https://api.anthropic.com");
    while (u->n && u->p[u->n - 1] == '/') u->n--;
    buf_puts(u, path);
}

static void headers(strv *h, bool fallbacks)
{
    buf auth = { 0 };
    buf_printf(&auth, "x-api-key: %s", getenv("ANTHROPIC_API_KEY"));
    strv_push(h, xstrdup("content-type: application/json"));
    strv_push(h, xstrdup("anthropic-version: 2026-10-21"));
    strv_push(h, xstrdup(buf_cstr(&auth)));
    if (fallbacks) strv_push(h, xstrdup("anthropic-beta: server-side-fallback-2026-07-01"));
    buf_free(&auth);
}

static bool mentions_overflow(const char *message)
{
    return strstr(message, "prompt is too long") || strstr(message, "context limit") || strstr(message, "context window");
}

static const char *error_kind(long s, json type, const char *message)
{
    if (json_eq(type, "authentication_error") || json_eq(type, "permission_error") || s == 401 || s == 403) return "auth";
    if (json_eq(type, "rate_limit_error") || s == 429) return "rate_limit";
    if (json_eq(type, "overloaded_error") || s == 529) return "overloaded";
    if (json_eq(type, "request_too_large") || s == 413 || mentions_overflow(message)) return "context_overflow";
    if (json_eq(type, "invalid_request_error") || json_eq(type, "not_found_error") || (s >= 400 && s < 500)) return "invalid_request";
    return "other";
}

static http_failure api_failure(long status, json body)
{
    json error = json_get(body, "error");
    json type = json_get(error, "type"), message = json_get(error, "message");
    char *text = json_is(message, '"') ? json_text_dup(message) : xstrdup("unexpected error response");
    const char *kind = error_kind(status, type, text), *reason;
    bool retryable = !strcmp(kind, "rate_limit") || !strcmp(kind, "overloaded") || status >= 500
                  || json_eq(type, "api_error");
    block_at at = NO_BLOCK;
    if (!strcmp(kind, "invalid_request") && messages_blame(text, &at, &reason)) {
        kind = "invalid_block";
        memmove(text, reason, strlen(reason) + 1);
    }
    return (http_failure){ kind, text, retryable, at };
}

static const http_stream_api MESSAGES_API = { messages_event, messages_complete, api_failure, "message_stop" };

static void require_key(void)
{
    if (!getenv("ANTHROPIC_API_KEY")) provider_error("auth", "ANTHROPIC_API_KEY is not set", 0);
}

static int list_models(void)
{
    strv h = { 0 };
    buf after = { 0 };
    require_key();
    headers(&h, false);
    for (bool more = true; more;) {
        buf u = { 0 };
        url(&u, "/v1/models?limit=1000");
        if (after.n) buf_printf(&u, "&after_id=%s", buf_cstr(&after));
        json page = http_get(buf_cstr(&u), &h, api_failure), listed;
        jiter it = json_iter(json_get(page, "data"));
        while (json_next(&it, NULL, &listed)) {
            char *id = json_text_dup(json_get(listed, "id"));
            if (id) provider_model(id, (long long)json_num(json_get(listed, "max_input_tokens"), 0),
                                   (long long)json_num(json_get(listed, "max_tokens"), 0));
            free(id);
        }
        after.n = 0;
        more = json_true(json_get(page, "has_more")) && json_text(json_get(page, "last_id"), &after) && after.n;
        buf_free(&u);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--models")) return list_models();
    json request = provider_request();
    bool fallbacks = !off(json_get(request, "params"), "fallbacks");
    json_buf body = { 0 };
    build_request(&body, request, fallbacks);
    require_key();
    buf u = { 0 };
    strv h = { 0 };
    url(&u, "/v1/messages");
    headers(&h, fallbacks);
    messages_result(!http_stream(buf_cstr(&u), &h, buf_span(&body), &MESSAGES_API));
}
