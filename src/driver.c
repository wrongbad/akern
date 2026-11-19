#include "driver.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "calls.h"
#include "inbox.h"
#include "msg.h"
#include "proc.h"
#include "session.h"
#include "sink.h"

static const char *const state_names[] = { "idle", "stream", "aborting", "waiting" };

enum call_results { WITHOUT_CALL_RESULTS, WITH_CALL_RESULTS };
enum assistant_end { REPLY_COMPLETE, REPLY_ABORTED };

static const unsigned WHEN_ANY = WHEN_NOW | WHEN_TURN | WHEN_IDLE;

enum { MAX_BLOCK_RETRIES = 8 };

typedef struct held_entry {
    char     *type, *parent_id;
    json_list members;
    json_buf  ref;
} held_entry;

static void report_error(driver *d, const char *where, span message)
{
    d->error.n = 0;
    json_printf(&d->error, "{'where':%s,'message':%S}", where, message);
    event_error(d->sink, where, message);
}

static void append_message(driver *d, json message, const char *source, json meta)
{
    json_list members = { 0 };
    json_printf(&members, "'message':%R", message);
    if (meta.n) json_printf(&members, ",'meta':%R", meta);
    const char *id = session_append(d->session, "message", AT_HEAD, &members);
    event(d->sink, "{'type':'message','run':%d,'id':%s,'source':%s,'message':%R}",
          d->run, id, source, message);
    buf_free(&members);
}

static void append_user_message(driver *d, unsigned when_mask, enum call_results call_results)
{
    json_list blocks = { 0 };
    json_buf message = { 0 };
    if (call_results == WITH_CALL_RESULTS) calls_take_turn_results(d->calls, &blocks);
    bool has_results = blocks.n > 0;
    size_t inbox_items = inbox_take(d->inbox, when_mask, &blocks);
    if (blocks.n) {
        message_user(&message, &blocks);
        append_message(d, buf_span(&message),
                       has_results ? (inbox_items ? "results+inbox" : "results") : "inbox", NOSPAN);
    }
    buf_free(&blocks);
    buf_free(&message);
}

static void start_run(driver *d)
{
    d->run++;
    d->turn = 0;
    d->block_retries = 0;
    d->error.n = d->answer_text.n = 0;
    event(d->sink, "{'type':'run_start','run':%d}", d->run);
}

static void end_run(driver *d, const char *reason)
{
    bool failed = strcmp(reason, "error") == 0;
    sink_answer(d->sink, buf_span(&d->answer_text), failed ? buf_span(&d->error) : NOSPAN);
    event(d->sink, "{'type':'run_end','run':%d,'reason':%s,'returned':[%r],'error':%R}",
          d->run, reason, buf_cstr(&d->returned_contents), failed ? buf_span(&d->error) : NOSPAN);
    d->last_run_failed = failed;
    d->returned_contents.n = 0;
    d->state = S_IDLE;
}

static json provider_usage(const driver *d)
{
    return json_get(buf_span(&d->provider_result), "usage");
}

static void end_turn(driver *d)
{
    event(d->sink, "{'type':'turn_end','run':%d,'turn':%d,'usage':%R}", d->run, d->turn, provider_usage(d));
}

static char *read_file(const char *path)
{
    buf text = { 0 };
    char chunk[8192];
    int fd = path ? open(path, O_RDONLY | O_CLOEXEC) : -1;
    for (ssize_t n; fd >= 0 && (n = read(fd, chunk, sizeof chunk)) > 0;) buf_put(&text, chunk, (size_t)n);
    if (fd >= 0) close(fd);
    return fd >= 0 ? buf_cstr(&text) : NULL;
}

static const char *model_id(const driver *d)
{
    const char *slash = strchr(d->provider_model, '/');
    return slash ? slash + 1 : "";
}

static char *provider_path(const driver *d)
{
    span provider = { d->provider_model, strcspn(d->provider_model, "/") };
    buf path = { 0 };
    buf_printf(&path, "%s/providers/%.*s", d->config->home_dir, (int)provider.n, provider.p);
    if (provider.n && provider.p[0] != '.' && access(path.p, X_OK) == 0) return path.p;
    buf_free(&path);
    return NULL;
}

static void build_request(driver *d, json_buf *request)
{
    json_buf messages = { 0 };
    char *system = read_file(d->system_path);
    strv_free(&d->context_ids);
    session_context(d->session, &messages, &d->context_ids);
    json_printf(request, "{'model':%s,'system':%s,'messages':%R,'tools':%R,'params':{%r}}",
                model_id(d), system, buf_span(&messages), calls_tool_schemas(d->calls),
                buf_cstr(&d->param_members));
    free(system);
    buf_free(&messages);
}

static void on_provider_line(worker *w, enum stream s, span line)
{
    driver *d = w->owner;
    if (s == ERR) {
        event_notice(d->sink, "provider", line);
        return;
    }
    if (!json_valid(line)) {
        event_error(d->sink, "provider", S("invalid line from provider"));
        return;
    }
    json type = json_get(line, "type");
    if (json_eq(type, "result")) buf_set(&d->provider_result, line);
    else if (json_eq(type, "error")) buf_set(&d->error, line), event_raw(d->sink, line);
    else event_raw(d->sink, line);
}

static void on_provider_exit(worker *w)
{
    driver *d = w->owner;
    d->provider = NULL;
    d->provider_exited = true;
}

static const worker_ops provider_ops = { NULL, on_provider_line, on_provider_exit };

static void spawn_provider(driver *d, json request)
{
    char *path = provider_path(d);
    if (!path) {
        buf why = { 0 };
        buf_printf(&why, "no provider for %s in %s/providers", d->provider_model, d->config->home_dir);
        report_error(d, "provider", buf_span(&why));
        buf_free(&why);
        end_run(d, "error");
        return;
    }
    spawn_req r = { .path = path, .stdin_bytes = request, .ops = &provider_ops, .owner = d };
    d->provider_result.n = d->error.n = 0;
    d->provider_exited = false;
    d->provider = proc_spawn(d->procs, &r);
    d->state = S_STREAM;
    event(d->sink, "{'type':'turn_start','run':%d,'turn':%d}", d->run, d->turn);
    free(path);
}

static void begin_turn(driver *d)
{
    json_buf request = { 0 };
    d->turn++;
    build_request(d, &request);
    spawn_provider(d, buf_span(&request));
    buf_free(&request);
}

static void continue_run_with_inbox(driver *d, unsigned when_mask)
{
    append_user_message(d, when_mask, WITHOUT_CALL_RESULTS);
    begin_turn(d);
}

static void append_entry(driver *d, const char *type, const char *parent_id, const json_list *members, json ref)
{
    const char *id = session_append(d->session, type, parent_id, members);
    if (id && ref.p) event(d->sink, "{'type':'ok','id':%s,'ref':%R}", id, ref);
}

static void append_held_entries(driver *d)
{
    for (size_t i = 0; i < d->nheld; i++) {
        held_entry *h = &d->held[i];
        append_entry(d, h->type, h->parent_id, &h->members, h->ref.n ? buf_span(&h->ref) : NOSPAN);
        free(h->type);
        free(h->parent_id);
        buf_free(&h->members);
        buf_free(&h->ref);
    }
    d->nheld = 0;
}

static bool settle(driver *d, const char *reason)
{
    append_held_entries(d);
    if (inbox_due(d->inbox, WHEN_ANY)) continue_run_with_inbox(d, WHEN_ANY);
    else end_run(d, reason);
    return true;
}

static void remember_answer(driver *d, json message)
{
    json_list other_blocks = { 0 };
    d->answer_text.n = 0;
    content_split_text(json_get(message, "content"), &d->answer_text, &other_blocks);
    buf_free(&other_blocks);
}

static json append_assistant_message(driver *d, enum assistant_end end)
{
    bool aborted = end == REPLY_ABORTED;
    json result = buf_span(&d->provider_result), message = json_get(result, "message");
    if (aborted) {
        d->stripped_message.n = 0;
        message = message_strip_tool_use(message, &d->stripped_message) ? buf_span(&d->stripped_message) : NOSPAN;
    }
    if (!message.p || message_empty(message)) return NOSPAN;
    json_buf meta = { 0 };
    json_printf(&meta, "{'model':%s,'stop':%R,'run':%d,'turn':%d,'usage':%R}", d->provider_model,
                aborted ? S("\"aborted\"") : json_get(result, "stop"), d->run, d->turn, provider_usage(d));
    append_message(d, message, "assistant", buf_span(&meta));
    buf_free(&meta);
    remember_answer(d, message);
    return message;
}

static bool idle(driver *d)
{
    append_held_entries(d);
    if (!inbox_due(d->inbox, WHEN_ANY)) return false;
    start_run(d);
    continue_run_with_inbox(d, WHEN_ANY);
    return true;
}

static bool omit_invalid_block(driver *d)
{
    json error = buf_span(&d->error), at = json_get(error, "at");
    double index = json_num(json_get(at, "message"), -1);
    if (!json_eq(json_get(error, "kind"), "invalid_block") || d->block_retries >= MAX_BLOCK_RETRIES
        || index < 0 || index >= (double)d->context_ids.n)
        return false;
    const char *target = d->context_ids.v[(size_t)index];
    json_buf message = { 0 }, omitted = { 0 };
    buf reason = { 0 };
    json_text(json_get(error, "message"), &reason);
    bool found = session_message(d->session, target, &message)
              && message_omit_media(buf_span(&message), (long)json_num(json_get(at, "block"), -1),
                                    (long)json_num(json_get(at, "inner"), -1), buf_span(&reason), &omitted);
    if (found) {
        json_list members = { 0 };
        json_printf(&members, "'target':%s,'message':%R", target, buf_span(&omitted));
        const char *id = session_append(d->session, "replace", AT_HEAD, &members);
        event(d->sink, "{'type':'replace','run':%d,'id':%s,'target':%s,'message':%R}",
              d->run, id, target, buf_span(&omitted));
        d->block_retries++;
        buf_free(&members);
    }
    buf_free(&message);
    buf_free(&omitted);
    buf_free(&reason);
    return found;
}

static bool streamed(driver *d)
{
    d->provider_exited = false;
    if (!d->provider_result.n && omit_invalid_block(d)) {
        begin_turn(d);
        return true;
    }
    if (!d->provider_result.n) {
        if (!d->error.n) report_error(d, "provider", S("provider exited without a result"));
        return settle(d, "error");
    }
    d->error.n = 0;
    d->block_retries = 0;
    json message = append_assistant_message(d, REPLY_COMPLETE);
    bool wants_tools = json_eq(json_get(buf_span(&d->provider_result), "stop"), "tool_use");
    if (wants_tools && calls_start(d->calls, message, d->run, d->turn)) {
        d->state = S_WAITING;
        inbox_mark(d->inbox);
        return true;
    }
    end_turn(d);
    return settle(d, "done");
}

static bool aborted(driver *d)
{
    d->provider_exited = false;
    if (d->provider_result.n) append_assistant_message(d, REPLY_ABORTED);
    if (d->abort_ends_run) end_run(d, "aborted");
    else continue_run_with_inbox(d, WHEN_NOW | WHEN_TURN);
    return true;
}

static bool resolved(driver *d)
{
    append_user_message(d, WHEN_NOW | WHEN_TURN, WITH_CALL_RESULTS);
    end_turn(d);
    begin_turn(d);
    return true;
}

static bool preempt(driver *d)
{
    event(d->sink, "{'type':'preempt','run':%d,'turn':%d}", d->run, d->turn);
    proc_term(d->provider);
    d->abort_ends_run = false;
    d->state = S_ABORTING;
    return true;
}

static void close_run(driver *d)
{
    if (d->state == S_WAITING) {
        calls_abort_turn(d->calls);
        append_user_message(d, 0, WITH_CALL_RESULTS);
    }
    end_run(d, "aborted");
}

static void return_prompts_and_defer_jobs(driver *d)
{
    inbox_return_prompts(d->inbox, WHEN_TURN | WHEN_IDLE, &d->returned_contents);
    inbox_defer_jobs(d->inbox);
}

static bool abort_run(driver *d)
{
    d->abort_requested = false;
    if (d->abort_kills_jobs) calls_kill_jobs(d->calls);
    if (d->state != S_IDLE) return_prompts_and_defer_jobs(d);
    switch (d->state) {
    case S_IDLE:
        break;
    case S_STREAM:
        proc_term(d->provider);
        d->state = S_ABORTING;
        d->abort_ends_run = true;
        break;
    case S_ABORTING:
        d->abort_ends_run = true;
        break;
    default:
        close_run(d);
    }
    return true;
}

static void use_model(driver *d, char *provider_model)
{
    free(d->provider_model);
    d->provider_model = provider_model;
    setenv("AKERN_MODEL", provider_model, 1);
}

void driver_open(driver *d)
{
    use_model(d, xstrdup(d->config->provider_model));
    d->system_path = xstrdup(d->config->system_path);
    buf_set(&d->param_members, buf_span(&d->config->param_members));
}

void driver_set(driver *d, json model, json system_path, json params)
{
    char *text;
    if ((text = json_text_dup(model))) use_model(d, text);
    if ((text = json_text_dup(system_path))) free(d->system_path), d->system_path = text;
    if (json_is(params, '{')) {
        json object = json_trim(params);
        buf_set(&d->param_members, json_trim((span){ object.p + 1, object.n - 2 }));
    }
}

bool driver_append(driver *d, const char *type, const char *parent_id, const json_list *members, json ref)
{
    if (parent_id == AT_HEAD) append_entry(d, type, AT_HEAD, members, ref);
    else if (*parent_id && !session_has_entry(d->session, parent_id)) return false;
    else {
        held_entry h = { .type = xstrdup(type), .parent_id = xstrdup(parent_id) };
        buf_set(&h.members, buf_span(members));
        buf_set(&h.ref, ref);
        VEC_PUSH(d->held, d->nheld, d->capheld, h);
    }
    return true;
}

bool driver_advance(driver *d)
{
    if (d->abort_requested) return abort_run(d);
    if (d->state == S_STREAM && inbox_due(d->inbox, WHEN_NOW)) return preempt(d);
    switch (d->state) {
    case S_IDLE:     return idle(d);
    case S_STREAM:   return d->provider_exited && streamed(d);
    case S_ABORTING: return d->provider_exited && aborted(d);
    case S_WAITING:
        return (calls_turn_settled(d->calls) || inbox_due_since_mark(d->inbox, WHEN_NOW | WHEN_TURN))
            && resolved(d);
    }
    return false;
}

void driver_abort(driver *d, enum abort_jobs jobs)
{
    d->abort_requested = true;
    d->abort_kills_jobs = jobs == KILL_JOBS;
}

void driver_stop(driver *d)
{
    if (d->state == S_IDLE) return;
    return_prompts_and_defer_jobs(d);
    close_run(d);
}

void driver_append_leftover_results(driver *d)
{
    json_list blocks = { 0 };
    json_buf message = { 0 };
    calls_close(d->calls);
    inbox_take_jobs(d->inbox, &blocks);
    if (blocks.n) {
        message_user(&message, &blocks);
        append_message(d, buf_span(&message), "inbox", NOSPAN);
    }
    buf_free(&blocks);
    buf_free(&message);
}

void driver_status(const driver *d, json_buf *status)
{
    json_buf turn = { 0 }, jobs = { 0 };
    calls_turn_json(d->calls, &turn);
    calls_jobs_json(d->calls, &jobs);
    json_printf(status,
                "{'type':'status','state':%s,'run':%d,'turn':%d,'model':%s,'head':%s,'inbox':%d,'calls':%R,'jobs':%R}",
                state_names[d->state], d->run, d->turn, d->provider_model, session_head_id(d->session),
                (int)inbox_size(d->inbox), buf_span(&turn), buf_span(&jobs));
    buf_free(&turn);
    buf_free(&jobs);
}

bool driver_quiescent(const driver *d)
{
    return d->state == S_IDLE && !d->abort_requested && !inbox_due(d->inbox, WHEN_ANY)
        && calls_running_jobs(d->calls) == 0;
}

bool driver_ran(const driver *d)
{
    return d->run > 0;
}

int driver_exit_code(const driver *d)
{
    return d->last_run_failed ? 1 : 0;
}
