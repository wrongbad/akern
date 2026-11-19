#include "lib/provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { DELTA_BYTES = 8, MAX_ECHOED_RESULT_BYTES = 400 };
static const double PAUSE_TICK_SECONDS = 0.01, FILE_POLL_SECONDS = 0.02;

static buf streamed_text;

static void finish_aborted(void)
{
    if (streamed_text.n)
        provider_emit("{'type':'result','stop':'aborted','message':{'role':'assistant','content':[{'type':'text','text':%S}]}}",
             buf_span(&streamed_text));
    else provider_emit("{'type':'result','stop':'aborted','message':{'role':'assistant','content':[]}}");
    exit(0);
}

static void pause_for(double seconds)
{
    for (double t = 0; t < seconds; t += PAUSE_TICK_SECONDS) {
        if (provider_aborted()) finish_aborted();
        nanosleep(&(struct timespec){ 0, (long)(PAUSE_TICK_SECONDS * 1e9) }, NULL);
    }
}

static void wait_for_file(const char *path)
{
    while (access(path, F_OK) != 0) pause_for(FILE_POLL_SECONDS);
}

static buf read_all(FILE *f)
{
    buf bytes = { 0 };
    char chunk[65536];
    for (size_t n; (n = fread(chunk, 1, sizeof chunk, f)) > 0;) buf_put(&bytes, chunk, n);
    return bytes;
}

static int count_role(json messages, const char *role)
{
    int n = 0;
    jiter it = json_iter(messages);
    for (json message; json_next(&it, NULL, &message);) n += json_eq(json_get(message, "role"), role);
    return n;
}

static void last_message_texts(json messages, buf *text, buf *result_text)
{
    json message, last = NOSPAN;
    jiter it = json_iter(messages);
    while (json_next(&it, NULL, &message)) last = message;
    jiter blocks = json_iter(json_get(last, "content"));
    for (json block; json_next(&blocks, NULL, &block);) {
        json type = json_get(block, "type");
        if (json_eq(type, "text")) text->n = 0, json_text(json_get(block, "text"), text);
        if ((json_eq(type, "tool_result") || json_eq(type, "job_result")) && !result_text->n) {
            jiter inner = json_iter(json_get(block, "content"));
            json first;
            if (json_next(&inner, NULL, &first)) json_text(json_get(first, "text"), result_text);
        }
    }
}

static void stream_text(span text)
{
    for (size_t i = 0; i < text.n; i += DELTA_BYTES) {
        span piece = { text.p + i, text.n - i < DELTA_BYTES ? text.n - i : DELTA_BYTES };
        provider_emit("{'type':'text_delta','text':%S}", piece);
        buf_putspan(&streamed_text, piece);
    }
}

static void echo_turn(json messages)
{
    buf text = { 0 }, result_text = { 0 }, reply = { 0 };
    last_message_texts(messages, &text, &result_text);
    if (text.n && text.p[0] == '!') {
        json_list blocks = { 0 };
        int n = 0;
        for (char *save, *cmd = strtok_r(buf_cstr(&text) + 1, ";", &save); cmd; cmd = strtok_r(NULL, ";", &save)) {
            if (n) buf_putc(&blocks, ',');
            if (cmd[0] == '@') {
                size_t name_len = strcspn(cmd + 1, " ");
                json input = json_trim(S(cmd + 1 + name_len));
                json_printf(&blocks, "{'type':'tool_use','id':'tu_%d_%d','name':%S,'input':%R}",
                     (int)getpid(), ++n, (span){ cmd + 1, name_len }, input.n ? input : S("{}"));
            } else {
                json_printf(&blocks, "{'type':'tool_use','id':'tu_%d_%d','name':'exec','input':{'argv':['sh','-c',%s]}}",
                     (int)getpid(), ++n, cmd);
            }
        }
        provider_emit("{'type':'result','stop':'tool_use','message':{'role':'assistant','content':[%r]},"
             "'usage':{'input':1,'output':1,'cache_read':0,'cache_write':0}}", buf_cstr(&blocks));
        return;
    }
    if (result_text.n > MAX_ECHOED_RESULT_BYTES) result_text.n = MAX_ECHOED_RESULT_BYTES;
    if (result_text.n) buf_printf(&reply, "The tool said: %s", buf_cstr(&result_text));
    else buf_printf(&reply, "echo: %s", buf_cstr(&text));
    if (text.n && result_text.n) buf_printf(&reply, " (and you said: %s)", text.p);
    stream_text(buf_span(&reply));
    provider_emit("{'type':'result','stop':'end_turn','message':{'role':'assistant','content':[{'type':'text','text':%S}]},"
         "'usage':{'input':1,'output':1,'cache_read':0,'cache_write':0}}", buf_span(&reply));
}

static void scripted_turn(const char *path, int turn)
{
    FILE *f = fopen(path, "r");
    if (!f) provider_error("other", "cannot open MOCK_SCRIPT", 0);
    buf script = read_all(f);
    fclose(f);
    int script_turn = 0;
    for (char *save, *text = strtok_r(buf_cstr(&script), "\n", &save); text; text = strtok_r(NULL, "\n", &save)) {
        json line = json_trim(S(text));
        json type = json_get(line, "type"), sleep_seconds = json_get(line, "x-sleep"), gate_path = json_get(line, "x-wait");
        bool is_result = json_eq(type, "result");
        if (script_turn == turn) {
            if (sleep_seconds.p) pause_for(json_num(sleep_seconds, 0));
            else if (gate_path.p) {
                char *path = json_text_dup(gate_path);
                wait_for_file(path);
                free(path);
            }
            else printf("%.*s\n", (int)line.n, line.p), fflush(stdout);
            if (json_eq(type, "text_delta")) json_text(json_get(line, "text"), &streamed_text);
            if (is_result) exit(0);
        }
        script_turn += is_result;
    }
    provider_emit("{'type':'result','stop':'end_turn','message':{'role':'assistant','content':[{'type':'text','text':'(script exhausted)'}]}}");
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--models") == 0) {
        provider_model("echo", 1000000, 1000);
        return 0;
    }
    json messages = json_get(provider_request(), "messages");
    const char *script = getenv("MOCK_SCRIPT");
    if (script && *script) scripted_turn(script, count_role(messages, "assistant"));
    else echo_turn(messages);
    return 0;
}
