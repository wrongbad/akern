#include "turns.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "calls.h"
#include "inbox.h"
#include "message.h"
#include "output.h"
#include "proc.h"
#include "session.h"

static const char *const state_names[] = {"idle", "stream", "aborting", "waiting"};

enum call_results { WITHOUT_CALL_RESULTS, WITH_CALL_RESULTS };
enum assistant_end { REPLY_COMPLETE, REPLY_ABORTED };

static const unsigned WHEN_ANY = WHEN_NOW | WHEN_TURN | WHEN_IDLE;

enum { MAX_BLOCK_RETRIES = 8 };

typedef struct held_entry {
    char *type, *parent_id;
    json_list members;
    json_buf ref;
} held_entry;

static void report_error(turns *d, const char *where, span message)
{
    d->error.n = 0;
    json_printf(&d->error, "{'where':%s,'message':%S}", where, message);
    output_emit_error(d->output, where, message);
}

static void append_message(turns *d, json message, const char *source, json meta)
{
    json_list members = {0};
    json_printf(&members, "'message':%R", message);
    if (meta.n)
        json_printf(&members, ",'meta':%R", meta);
    const char *id = session_append(d->session, "message", AT_HEAD, &members);
    output_emit(
        d->output,
        "{'type':'message','run':%d,'id':%s,'source':%s,'message':%R}",
        d->run,
        id,
        source,
        message
    );
    buf_free(&members);
}

static void append_user_message(turns *d, unsigned when_mask, enum call_results call_results)
{
    json_list blocks = {0};
    json_buf message = {0};
    if (call_results == WITH_CALL_RESULTS)
        calls_take_turn_results(d->calls, &blocks);
    bool has_results = blocks.n > 0;
    size_t inbox_items = inbox_take(d->inbox, when_mask, &blocks);
    if (blocks.n)
    {
        message_put_user(&message, &blocks);
        append_message(
            d,
            buf_span(&message),
            has_results ? (inbox_items ? "results+inbox" : "results") : "inbox",
            NOSPAN
        );
    }
    buf_free(&blocks);
    buf_free(&message);
}

static void start_run(turns *d)
{
    d->run++;
    d->turn = 0;
    d->block_retries = 0;
    d->error.n = d->answer_text.n = 0;
    output_emit(d->output, "{'type':'run_start','run':%d}", d->run);
}

static void end_run(turns *d, const char *reason)
{
    bool failed = strcmp(reason, "error") == 0;
    output_write_answer(
        d->output,
        buf_span(&d->answer_text),
        failed ? buf_span(&d->error) : NOSPAN
    );
    output_emit(
        d->output,
        "{'type':'run_end','run':%d,'reason':%s,'returned':[%r],'error':%R}",
        d->run,
        reason,
        buf_cstr(&d->returned_contents),
        failed ? buf_span(&d->error) : NOSPAN
    );
    d->last_run_failed = failed;
    d->returned_contents.n = 0;
    d->state = S_IDLE;
}

static json provider_usage(const turns *d)
{
    return json_get(buf_span(&d->provider_result), "usage");
}

static void end_turn(turns *d)
{
    output_emit(
        d->output,
        "{'type':'turn_end','run':%d,'turn':%d,'usage':%R}",
        d->run,
        d->turn,
        provider_usage(d)
    );
}

static bool read_file(const char *path, buf *text)
{
    char chunk[8192];
    ssize_t n = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    while (fd >= 0 && (n = read(fd, chunk, sizeof chunk)) > 0)
        buf_put(text, chunk, (size_t)n);
    int error = errno;
    if (fd >= 0)
        close(fd);
    errno = error;
    return fd >= 0 && n == 0;
}

static const char *model_id(const turns *d)
{
    const char *slash = strchr(d->provider_model, '/');
    return slash ? slash + 1 : "";
}

static char *home_program(const turns *d, span name)
{
    static const char *const layers[] = {
        "",
        "/system"
    }; /* the user's overlay, then what make install owns */
    for (size_t i = 0; i < LEN(layers); i++)
    {
        buf path = {0};
        buf_printf(&path, "%s%s/%.*s", d->config->home_dir, layers[i], (int)name.n, name.p);
        if (access(path.p, X_OK) == 0)
            return path.p;
        buf_free(&path);
    }
    return NULL;
}

static char *provider_path(const turns *d)
{
    span provider = {d->provider_model, strcspn(d->provider_model, "/")};
    if (!provider.n || provider.p[0] == '.')
        return NULL;
    buf name = {0};
    buf_printf(&name, "providers/%.*s", (int)provider.n, provider.p);
    char *path = home_program(d, buf_span(&name));
    buf_free(&name);
    return path;
}

static void run_prompt_program(turns *d)
{
    char *path = home_program(d, S("system-prompt"));
    if (!path)
        return;
    int wait_status = proc_run(path, &d->startup_system);
    if (wait_status)
    {
        buf why = {0};
        proc_describe_status(wait_status, &why);
        die("%s: %s", path, buf_cstr(&why));
    }
    free(path);
}

static void build_request(turns *d, json_buf *request)
{
    json_buf messages = {0};
    strv_free(&d->context_ids);
    session_read_context(d->session, &messages, &d->context_ids);
    json_printf(
        request,
        "{'model':%s,'session':%s,'system':%S,'messages':%R,'tools':%R,'params':{%r}}",
        model_id(d),
        d->config->session_path,
        d->system.n ? buf_span(&d->system) : NOSPAN,
        buf_span(&messages),
        calls_get_tool_schemas(d->calls),
        buf_cstr(&d->param_members)
    );
    buf_free(&messages);
}

static void on_provider_line(worker *w, enum stream s, span line)
{
    turns *d = w->owner;
    if (s == ERR)
    {
        output_emit_notice(d->output, "provider", line);
        return;
    }
    if (!json_is_valid(line))
    {
        output_emit_error(d->output, "provider", S("invalid line from provider"));
        return;
    }
    json type = json_get(line, "type");
    if (json_eq(type, "result"))
        buf_set(&d->provider_result, line);
    else if (json_eq(type, "error"))
        buf_set(&d->error, line), output_emit_raw(d->output, line);
    else
        output_emit_raw(d->output, line);
}

static void on_provider_exit(worker *w)
{
    turns *d = w->owner;
    d->provider = NULL;
    d->provider_exited = true;
}

static const worker_ops provider_ops = {NULL, on_provider_line, on_provider_exit};

static void spawn_provider(turns *d, json request)
{
    char *path = d->provider_model ? provider_path(d) : NULL;
    if (!d->provider_model)
    {
        report_error(d, "provider", S("no model: append a settings entry with a model"));
        end_run(d, "error");
        return;
    }
    if (!path)
    {
        buf why = {0};
        buf_printf(
            &why,
            "no provider for %s in %s/providers or %s/system/providers",
            d->provider_model,
            d->config->home_dir,
            d->config->home_dir
        );
        report_error(d, "provider", buf_span(&why));
        buf_free(&why);
        end_run(d, "error");
        return;
    }
    spawn_req r = {.path = path, .stdin_bytes = request, .ops = &provider_ops, .owner = d};
    d->provider_result.n = d->error.n = 0;
    d->provider_exited = false;
    d->provider = proc_spawn(d->procs, &r);
    d->state = S_STREAM;
    output_emit(d->output, "{'type':'turn_start','run':%d,'turn':%d}", d->run, d->turn);
    free(path);
}

static void begin_turn(turns *d)
{
    json_buf request = {0};
    d->turn++;
    build_request(d, &request);
    spawn_provider(d, buf_span(&request));
    buf_free(&request);
}

static void continue_run_with_inbox(turns *d, unsigned when_mask)
{
    append_user_message(d, when_mask, WITHOUT_CALL_RESULTS);
    begin_turn(d);
}

/* Overwrites the fields that one settings entry names. */
static void apply_settings_entry(turns *d, json entry)
{
    json model = json_get(entry, "model"), system = json_get(entry, "system");
    json params = json_get(entry, "params");
    char *text = json_decode_dup(model);
    if (text)
        free(d->provider_model), d->provider_model = text;
    if (json_is(system, '"'))
        d->system.n = 0, json_decode(system, &d->system);
    if (json_is(params, '{'))
    {
        json object = json_trim(params);
        buf_set(&d->param_members, json_trim((span){object.p + 1, object.n - 2}));
    }
}

/* The settings in effect: the startup ones (or none, without from_startup), then every
   settings entry on the chain, oldest first. */
static void fold_settings(turns *d, bool from_startup)
{
    const config *cfg = d->config;
    free(d->provider_model);
    d->provider_model = from_startup && cfg->provider_model ? xstrdup(cfg->provider_model) : NULL;
    d->system.n = d->param_members.n = 0;
    if (from_startup)
    {
        buf_putspan(&d->system, buf_span(&d->startup_system));
        buf_putspan(&d->param_members, buf_span(&cfg->param_members));
    }
    json_list entries = {0};
    json_buf array = {0};
    session_read_settings(d->session, &entries);
    json_printf(&array, "[%r]", buf_cstr(&entries));
    jiter it = json_iter(buf_span(&array));
    for (json entry; json_next(&it, NULL, &entry);)
        apply_settings_entry(d, entry);
    if (d->provider_model)
        setenv("AKERN_MODEL", d->provider_model, 1);
    buf_free(&entries);
    buf_free(&array);
}

static void append_entry(
    turns *d,
    const char *type,
    const char *parent_id,
    const json_list *members,
    json ref
)
{
    const char *id = session_append(d->session, type, parent_id, members);
    if (id)
        fold_settings(d, true);
    if (id && ref.p)
        output_emit(d->output, "{'type':'ok','id':%s,'ref':%R}", id, ref);
}

static void append_held_entries(turns *d)
{
    for (size_t i = 0; i < d->nheld; i++)
    {
        held_entry *h = &d->held[i];
        append_entry(d, h->type, h->parent_id, &h->members, h->ref.n ? buf_span(&h->ref) : NOSPAN);
        free(h->type);
        free(h->parent_id);
        buf_free(&h->members);
        buf_free(&h->ref);
    }
    d->nheld = 0;
}

static bool settle(turns *d, const char *reason)
{
    append_held_entries(d);
    if (inbox_has_due(d->inbox, WHEN_ANY))
        continue_run_with_inbox(d, WHEN_ANY);
    else
        end_run(d, reason);
    return true;
}

static void remember_answer(turns *d, json message)
{
    json_list other_blocks = {0};
    d->answer_text.n = 0;
    content_split_text(json_get(message, "content"), &d->answer_text, &other_blocks);
    buf_free(&other_blocks);
}

static json append_assistant_message(turns *d, enum assistant_end end)
{
    bool aborted = end == REPLY_ABORTED;
    json result = buf_span(&d->provider_result), message = json_get(result, "message");
    if (aborted)
    {
        d->stripped_message.n = 0;
        message = message_strip_tool_use(message, &d->stripped_message)
                      ? buf_span(&d->stripped_message)
                      : NOSPAN;
    }
    if (!message.p || message_is_empty(message))
        return NOSPAN;
    json_buf meta = {0};
    json_printf(
        &meta,
        "{'model':%s,'stop':%R,'run':%d,'turn':%d,'usage':%R}",
        d->provider_model,
        aborted ? S("\"aborted\"") : json_get(result, "stop"),
        d->run,
        d->turn,
        provider_usage(d)
    );
    append_message(d, message, "assistant", buf_span(&meta));
    buf_free(&meta);
    remember_answer(d, message);
    return message;
}

static bool idle(turns *d)
{
    append_held_entries(d);
    if (!inbox_has_due(d->inbox, WHEN_ANY))
        return false;
    start_run(d);
    continue_run_with_inbox(d, WHEN_ANY);
    return true;
}

static bool omit_invalid_block(turns *d)
{
    json error = buf_span(&d->error), at = json_get(error, "at");
    double index = json_get_num(json_get(at, "message"), -1);
    bool names_block = json_eq(json_get(error, "kind"), "invalid_block");
    bool in_context = index >= 0 && index < (double)d->context_ids.n;
    if (!names_block || !in_context || d->block_retries >= MAX_BLOCK_RETRIES)
        return false;
    const char *target = d->context_ids.v[(size_t)index];
    json_buf message = {0}, omitted = {0};
    buf reason = {0};
    json_decode(json_get(error, "message"), &reason);
    bool found = session_read_message(d->session, target, &message) &&
                 message_omit_media(
                     buf_span(&message),
                     (long)json_get_num(json_get(at, "block"), -1),
                     (long)json_get_num(json_get(at, "inner"), -1),
                     buf_span(&reason),
                     &omitted
                 );
    if (found)
    {
        json_list members = {0};
        json_printf(&members, "'target':%s,'message':%R", target, buf_span(&omitted));
        const char *id = session_append(d->session, "replace", AT_HEAD, &members);
        output_emit(
            d->output,
            "{'type':'replace','run':%d,'id':%s,'target':%s,'message':%R}",
            d->run,
            id,
            target,
            buf_span(&omitted)
        );
        d->block_retries++;
        buf_free(&members);
    }
    buf_free(&message);
    buf_free(&omitted);
    buf_free(&reason);
    return found;
}

static bool streamed(turns *d)
{
    d->provider_exited = false;
    if (!d->provider_result.n && omit_invalid_block(d))
    {
        begin_turn(d);
        return true;
    }
    if (!d->provider_result.n)
    {
        if (!d->error.n)
            report_error(d, "provider", S("provider exited without a result"));
        return settle(d, "error");
    }
    d->error.n = 0;
    d->block_retries = 0;
    json message = append_assistant_message(d, REPLY_COMPLETE);
    bool wants_tools = json_eq(json_get(buf_span(&d->provider_result), "stop"), "tool_use");
    if (wants_tools && calls_start(d->calls, message, d->run, d->turn))
    {
        d->state = S_WAITING;
        inbox_mark(d->inbox);
        return true;
    }
    end_turn(d);
    return settle(d, "done");
}

static bool aborted(turns *d)
{
    d->provider_exited = false;
    if (d->provider_result.n)
        append_assistant_message(d, REPLY_ABORTED);
    if (d->abort_ends_run)
        end_run(d, "aborted");
    else
        continue_run_with_inbox(d, WHEN_NOW | WHEN_TURN);
    return true;
}

static bool resolved(turns *d)
{
    append_user_message(d, WHEN_NOW | WHEN_TURN, WITH_CALL_RESULTS);
    end_turn(d);
    begin_turn(d);
    return true;
}

static bool preempt(turns *d)
{
    output_emit(d->output, "{'type':'preempt','run':%d,'turn':%d}", d->run, d->turn);
    proc_term(d->provider);
    d->abort_ends_run = false;
    d->state = S_ABORTING;
    return true;
}

static void close_run(turns *d)
{
    if (d->state == S_WAITING)
    {
        calls_abort_turn(d->calls);
        append_user_message(d, 0, WITH_CALL_RESULTS);
    }
    end_run(d, "aborted");
}

static void return_prompts_and_defer_jobs(turns *d)
{
    inbox_return_prompts(d->inbox, WHEN_TURN | WHEN_IDLE, &d->returned_contents);
    inbox_defer_jobs(d->inbox);
}

static bool abort_run(turns *d)
{
    d->abort_requested = false;
    if (d->abort_kills_jobs)
        calls_kill_jobs(d->calls);
    if (d->state != S_IDLE)
        return_prompts_and_defer_jobs(d);
    switch (d->state)
    {
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

static bool is_same_span(span a, span b)
{
    return a.n == b.n && (!a.n || !memcmp(a.p, b.p, a.n));
}

/* The members of one settings entry: each startup setting that differs from what the session
   says. */
static void put_startup_changes(turns *d, json_list *members)
{
    const config *cfg = d->config;
    span model = cfg->provider_model ? S(cfg->provider_model) : NOSPAN;
    span current = d->provider_model ? S(d->provider_model) : NOSPAN;
    span system = buf_span(&d->startup_system), params = buf_span(&cfg->param_members);
    if (model.n && !is_same_span(model, current))
        json_printf(members, "'model':%S", model);
    if (system.n && !is_same_span(system, buf_span(&d->system)))
        json_printf(members, "%r'system':%S", members->n ? "," : "", system);
    if (params.n && !is_same_span(params, buf_span(&d->param_members)))
        json_printf(members, "%r'params':{%R}", members->n ? "," : "", params);
}

void turns_open(turns *d)
{
    const char *system_path = d->config->system_path;
    if (!system_path)
        run_prompt_program(d);
    else if (!read_file(system_path, &d->startup_system))
        die("%s: %s", system_path, strerror(errno));
}

void turns_apply_startup_settings(turns *d)
{
    fold_settings(d, false);
    json_list changes = {0};
    put_startup_changes(d, &changes);
    if (changes.n)
        session_append(d->session, "settings", AT_HEAD, &changes);
    buf_free(&changes);
    fold_settings(d, true);
    if (!d->provider_model)
        die("no model: pass -m provider/model, set AKERN_MODEL, or resume a session that has one");
}

bool turns_append(
    turns *d,
    const char *type,
    const char *parent_id,
    const json_list *members,
    json ref
)
{
    if (parent_id == AT_HEAD)
        append_entry(d, type, AT_HEAD, members, ref);
    else if (*parent_id && !session_has_entry(d->session, parent_id))
        return false;
    else
    {
        held_entry h = {.type = xstrdup(type), .parent_id = xstrdup(parent_id)};
        buf_set(&h.members, buf_span(members));
        buf_set(&h.ref, ref);
        VEC_PUSH(d->held, d->nheld, d->capheld, h);
    }
    return true;
}

bool turns_advance(turns *d)
{
    if (d->abort_requested)
        return abort_run(d);
    if (d->state == S_STREAM && inbox_has_due(d->inbox, WHEN_NOW))
        return preempt(d);
    switch (d->state)
    {
    case S_IDLE:
        return idle(d);
    case S_STREAM:
        return d->provider_exited && streamed(d);
    case S_ABORTING:
        return d->provider_exited && aborted(d);
    case S_WAITING:
        return (calls_is_turn_settled(d->calls) ||
                   inbox_has_due_since_mark(d->inbox, WHEN_NOW | WHEN_TURN)) &&
               resolved(d);
    }
    return false;
}

void turns_abort(turns *d, enum abort_jobs jobs)
{
    d->abort_requested = true;
    d->abort_kills_jobs = jobs == KILL_JOBS;
}

void turns_stop(turns *d)
{
    if (d->state == S_IDLE)
        return;
    return_prompts_and_defer_jobs(d);
    close_run(d);
}

void turns_append_leftover_results(turns *d)
{
    json_list blocks = {0};
    json_buf message = {0};
    calls_close(d->calls);
    inbox_take_jobs(d->inbox, &blocks);
    if (blocks.n)
    {
        message_put_user(&message, &blocks);
        append_message(d, buf_span(&message), "inbox", NOSPAN);
    }
    buf_free(&blocks);
    buf_free(&message);
}

void turns_put_status(const turns *d, json_buf *status)
{
    json_buf turn = {0}, jobs = {0};
    calls_put_turn_json(d->calls, &turn);
    calls_put_jobs_json(d->calls, &jobs);
    json_printf(
        status,
        "{'type':'status','state':%s,'run':%d,'turn':%d,'model':%s,'head':%s,'inbox':%d,'"
        "calls':%R,'jobs':%R}",
        state_names[d->state],
        d->run,
        d->turn,
        d->provider_model,
        session_get_head_id(d->session),
        (int)inbox_count(d->inbox),
        buf_span(&turn),
        buf_span(&jobs)
    );
    buf_free(&turn);
    buf_free(&jobs);
}

bool turns_is_quiescent(const turns *d)
{
    return d->state == S_IDLE && !d->abort_requested && !inbox_has_due(d->inbox, WHEN_ANY) &&
           calls_count_running_jobs(d->calls) == 0;
}

int turns_get_run(const turns *d)
{
    return d->run;
}

bool turns_has_run(const turns *d)
{
    return d->run > 0;
}

int turns_get_exit_code(const turns *d)
{
    return d->last_run_failed ? 1 : 0;
}
