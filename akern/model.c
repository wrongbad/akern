/* model: the actor that talks to the model, one request in flight at a time (providers.md). Owns
   the context, the messages since the last reset, rebuilt from the session file when resuming
   and kept from the lines on the bus, a reply where its turn_start was and a replace entry's
   message in its target's place; starting a turn when the inbox says continue, the request
   built from the context; running the provider and keeping one that stays; putting its deltas
   on the bus between turn_start and the reply's message, which it seals with the provider's
   result, byte for byte; stopping the provider on a preempt or abort line, keeping the partial
   reply; and closing every turn with turn_end as the provider finishes. When the provider names
   a request block the API rejected, puts a replace entry that swaps it for a note and, once that
   line comes back, sends the request again, a few times at most. The model, system text and
   params are fixed for the kernel's life: -m (or AKERN_MODEL), -S (else the system-prompt
   program's output) and -P; another model is another start. When a turn happens is the inbox's;
   running the tools is calls'.
   turn = one provider request and its reply, from turn_start to turn_end. */

#include "akern.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { MAX_BLOCK_RETRIES = 8 };

/* A message entry; an empty message holds the place of a reply whose turn has begun. */
typedef struct context_entry {
    char id[MAX_ID_CHARS + 1];
    json_buf message;
} context_entry;

static void report_error(model *m, const char *where, span message)
{
    m->error.n = 0;
    json_printf(&m->error, "{'where':%s,'message':%S}", where, message);
    bus_put_error(m->bus, where, message);
}

/* $AKERN_HOME/NAME if it is executable (the user's overlay), else $AKERN_HOME/system/NAME. */
static char *home_program(const model *m, const char *name)
{
    for (int system = 0; system < 2; system++)
    {
        buf path = {0};
        buf_printf(&path, "%s%s/%s", m->config->home_dir, system ? "/system" : "", name);
        if (access(path.p, X_OK) == 0)
            return path.p;
        buf_free(&path);
    }
    return NULL;
}

/* ---- the context ---- */

static void copy_id(json value, char id[MAX_ID_CHARS + 1])
{
    buf text = {0};
    id[0] = 0;
    if (json_decode(value, &text) && text.n <= MAX_ID_CHARS)
        memcpy(id, text.p, text.n + 1);
    buf_free(&text);
}

static context_entry *find(model *m, const char *id)
{
    for (size_t i = 0; *id && i < m->ncontext; i++)
        if (!strcmp(id, m->context[i].id))
            return &m->context[i];
    return NULL;
}

/* Drops the reply placeholders, or, for a reset, everything. */
static void drop(model *m, bool placeholders_only)
{
    size_t kept = 0;
    for (size_t i = 0; i < m->ncontext; i++)
        if (placeholders_only && m->context[i].message.n)
            m->context[kept++] = m->context[i];
        else
            buf_free(&m->context[i].message);
    m->ncontext = kept;
}

/* What a line does to the context, the same whether it is on the bus or in a resumed file: a
   reply sits where its turn began, and a replace entry's message stands in for its target's. */
void model_replay(void *ctx, json line)
{
    model *m = ctx;
    json type = json_get(line, "type"), entry = json_get(line, "ts");
    char id[MAX_ID_CHARS + 1];
    copy_id(json_get(line, json_eq(type, "replace") ? "target" : "id"), id);
    context_entry *e = find(m, id);
    if (json_eq(type, "turn_end"))
        drop(m, true); /* a turn that sealed no reply */
    else if (json_eq(type, "reset") && entry.p)
        drop(m, false);
    else if (json_eq(type, "replace") && e && json_is(json_get(line, "message"), '{'))
        buf_set(&e->message, json_get(line, "message"));
    else if ((json_eq(type, "turn_start") && !e) || (json_eq(type, "message") && entry.p))
    {
        if (!e)
        {
            VEC_PUSH(m->context, m->ncontext, m->capcontext, (context_entry){0});
            e = &m->context[m->ncontext - 1];
            memcpy(e->id, id, sizeof id);
        }
        buf_set(&e->message, json_eq(type, "message") ? json_get(line, "message") : NOSPAN);
    }
}

/* ---- messages ---- */

static bool strip_tool_use(json message, json_buf *stripped)
{
    json_list blocks = {0};
    jiter it = json_iter(json_get(message, "content"));
    for (json block; json_next(&it, NULL, &block);)
        if (!json_eq(json_get(block, "type"), "tool_use"))
            json_printf(&blocks, "%r%R", blocks.n ? "," : "", block);
    bool any = blocks.n > 0;
    if (any)
        json_printf(stripped, "{'role':'assistant','content':[%r]}", buf_cstr(&blocks));
    buf_free(&blocks);
    return any;
}

static bool is_empty(json message)
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

static bool omit_media(json message, long block, long inner, span reason, json_buf *out)
{
    long path[] = {block, inner, -1};
    return block >= 0 && put_without_media(out, message, path, reason);
}

/* ---- the provider ---- */

static void streamed(model *m, bool stayed);
static void stopped(model *m);

/* The provider's deltas and extensions go on the bus; any other type would be a line of the
   session, which only the kernel writes. Lines of a provider that is no longer the turn's are
   dropped; its stderr is a notice. */
static void on_provider_line(process *p, enum stream s, span line)
{
    model *m = p->owner;
    json type = json_get(line, "type");
    span name = json_get_escaped_body(type);
    if (s == OUT && p == m->provider && m->turn_open)
        m->provider_spoke = true;
    if (s == ERR)
        bus_printf(m->bus, "{'type':'notice','source':'provider','message':%S}", json_trim(line));
    else if (p != m->provider || !m->turn_open)
        return;
    else if (!json_is_valid(line))
        bus_put_error(m->bus, "provider", S("invalid line from provider"));
    else if (json_eq(type, "result"))
    {
        buf_set(&m->provider_result, line);
        if (!m->stopping && json_is_true(json_get(line, "stay")))
            streamed(m, true); /* it stays for the next request: done with this one */
    }
    else if (json_eq(type, "error"))
        buf_set(&m->error, line), bus_put(m->bus, line);
    else if (
        json_eq(type, "text_delta") || json_eq(type, "thinking_delta") ||
        json_eq(type, "tool_use_start") || json_eq(type, "tool_use_delta") ||
        (json_is(type, '"') && name.n > 2 && !memcmp(name.p, "x-", 2))
    )
        bus_put(m->bus, line);
}

static void on_provider_exit(process *p)
{
    model *m = p->owner;
    if (p != m->provider)
        return;
    m->provider = NULL;
    if (m->turn_open && m->stopping)
        stopped(m);
    else if (m->turn_open)
        streamed(m, false);
}

static const process_ops provider_ops = {NULL, on_provider_line, on_provider_exit};

/* Lets go of a provider that stays: with stdin closed, it exits on its own. */
static void release_provider(model *m)
{
    if (m->provider)
        proc_write_stdin(m->provider, NOSPAN, true);
    m->provider = NULL;
}

/* The request line, to the provider that stayed after the last turn, else to a new one. */
static void send_request(model *m, bool may_reuse)
{
    process *kept = m->provider;
    m->provider_result.n = m->error.n = 0;
    m->provider_spoke = false;
    m->provider_reused = may_reuse && kept && kept->kill_stage == NOT_KILLED && !kept->exited &&
                         proc_write_stdin(kept, buf_span(&m->request_line), false);
    if (m->provider_reused)
        return;
    release_provider(m);
    spawn_req r = {
        .path = m->provider_path,
        .stdin_bytes = buf_span(&m->request_line),
        .keep_stdin_open = true,
        .ops = &provider_ops,
        .owner = m
    };
    m->provider = proc_spawn(m->processes, &r);
}

/* ---- turns ---- */

static void close_turn(model *m, const char *reason)
{
    bus_printf(
        m->bus,
        "{'type':'turn_end','run':%d,'turn':%d,'reason':%s,'usage':%R,'error':%R}",
        m->run,
        m->turn,
        reason,
        json_get(buf_span(&m->provider_result), "usage"),
        !strcmp(reason, "error") && m->error.n ? buf_span(&m->error) : NOSPAN
    );
    m->turn_open = m->stopping = false;
}

/* A turn: the context as the request, one line (a newline in JSON is whitespace, since strings
   escape theirs); one that cannot start ends at once, in error. */
static void begin_turn(model *m)
{
    m->turn++;
    m->turn_open = true;
    m->provider_result.n = 0;
    if (!m->provider_path)
    {
        buf why = {0};
        const char *home = m->config->home_dir;
        buf_printf(
            &why,
            "no provider for %s in %s/providers or %s/system/providers",
            m->config->provider_model,
            home,
            home
        );
        report_error(m, "provider", buf_span(&why));
        buf_free(&why);
        close_turn(m, "error");
        return;
    }
    json_list messages = {0};
    strv_free(&m->context_ids);
    for (size_t i = 0; i < m->ncontext; i++)
        if (m->context[i].message.n)
        {
            json_printf(&messages, "%r%R", messages.n ? "," : "", buf_span(&m->context[i].message));
            strv_push(&m->context_ids, xstrdup(m->context[i].id));
        }
    m->request_line.n = 0;
    json_printf(
        &m->request_line,
        "{'model':%s,'session':%s,'system':%S,'messages':[%R],'tools':%R,'params':{%r}}",
        strchr(m->config->provider_model, '/') ? strchr(m->config->provider_model, '/') + 1 : "",
        m->config->session_path,
        m->system.n ? buf_span(&m->system) : NOSPAN,
        buf_span(&messages),
        buf_span(&m->tools->schemas),
        m->config->param_members.p ? m->config->param_members.p : ""
    );
    for (size_t i = 0; i < m->request_line.n; i++)
        if (m->request_line.p[i] == '\n' || m->request_line.p[i] == '\r')
            m->request_line.p[i] = ' ';
    buf_putc(&m->request_line, '\n');
    buf_free(&messages);
    send_request(m, true);
    bus_new_id(m->bus, m->turn_id);
    bus_printf(
        m->bus,
        "{'type':'turn_start','run':%d,'turn':%d,'id':%s}",
        m->run,
        m->turn,
        m->turn_id
    );
}

/* The reply, sealed: the provider's message byte for byte, or, cut short, without its tool_use
   blocks. Nothing when no content is left. */
static void seal_reply(model *m, bool aborted)
{
    json result = buf_span(&m->provider_result), message = json_get(result, "message");
    json_buf stripped = {0};
    if (aborted)
        message = strip_tool_use(message, &stripped) ? buf_span(&stripped) : NOSPAN;
    if (message.p && !is_empty(message))
        bus_printf(
            m->bus,
            "{'type':'message','id':%s,'ts':%D,'message':%R,'meta':{'model':%s,'stop':%R,'run':%d,"
            "'turn':%d,'usage':%R,'source':'assistant'}}",
            m->turn_id,
            get_unix_seconds(),
            message,
            m->config->provider_model,
            aborted ? S("\"aborted\"") : json_get(result, "stop"),
            m->run,
            m->turn,
            json_get(result, "usage")
        );
    buf_free(&stripped);
}

/* Puts a replace entry for the block the provider says the API rejected, so that the request
   can go again; false if the error names no such block or the retries are spent. */
static bool omit_invalid_block(model *m)
{
    json error = buf_span(&m->error), at = json_get(error, "at");
    double index = json_get_num(json_get(at, "message"), -1);
    if (!json_eq(json_get(error, "kind"), "invalid_block") || index < 0 ||
        index >= (double)m->context_ids.n || m->block_retries >= MAX_BLOCK_RETRIES)
        return false;
    const char *target = m->context_ids.v[(size_t)index];
    context_entry *e = find(m, target);
    json_buf omitted = {0};
    buf reason = {0};
    json_decode(json_get(error, "message"), &reason);
    long block = (long)json_get_num(json_get(at, "block"), -1),
         inner = (long)json_get_num(json_get(at, "inner"), -1);
    bool found = e && omit_media(buf_span(&e->message), block, inner, buf_span(&reason), &omitted);
    if (found)
    {
        char id[NEW_ID_CHARS + 1];
        bus_new_id(m->bus, id);
        bus_printf(
            m->bus,
            "{'type':'replace','id':%s,'ts':%D,'target':%s,'message':%R}",
            id,
            get_unix_seconds(),
            target,
            buf_span(&omitted)
        );
        m->block_retries++;
    }
    buf_free(&omitted), buf_free(&reason);
    return found;
}

/* The provider is done with the request: the reply, a retry, or a failure. */
static void streamed(model *m, bool stayed)
{
    if (!m->provider_result.n && !m->provider_spoke && m->provider_reused)
    {
        send_request(m, false); /* the provider that stayed ended before answering: a new one */
        return;
    }
    if (!stayed)
        release_provider(m);
    if (!m->provider_result.n && omit_invalid_block(m))
        m->retrying = true; /* the next request, once its replace line comes back */
    else if (!m->provider_result.n)
    {
        if (!m->error.n)
            report_error(m, "provider", S("provider exited without a result"));
        close_turn(m, "error");
    }
    else
    {
        m->error.n = 0;
        m->block_retries = 0;
        seal_reply(m, false);
        close_turn(m, "done");
    }
}

/* The provider stopped as asked: the partial reply, if any, sealed as aborted. */
static void stopped(model *m)
{
    m->retrying = false;
    if (m->provider_result.n)
        seal_reply(m, true);
    close_turn(m, "aborted");
}

/* ---- startup ---- */

void model_open(model *m)
{
    const char *system_path = m->config->system_path;
    if (!m->config->provider_model)
        die("no model: pass -m provider/model or set AKERN_MODEL");
    setenv("AKERN_MODEL", m->config->provider_model, 1);
    size_t name = strcspn(m->config->provider_model, "/");
    if (name && m->config->provider_model[0] != '.')
    {
        buf provider = {0};
        buf_printf(&provider, "providers/%.*s", (int)name, m->config->provider_model);
        m->provider_path = home_program(m, buf_cstr(&provider));
        buf_free(&provider);
    }
    char *program = NULL;
    if (system_path && !read_file(system_path, &m->system))
        die("%s: %s", system_path, strerror(errno));
    if (!system_path)
        program = home_program(m, "system-prompt"); /* -S, else this program's output */
    int wait_status = program ? proc_run(program, &m->system) : 0;
    if (wait_status)
    {
        buf why = {0};
        proc_describe_status(wait_status, &why);
        die("%s: %s", program, buf_cstr(&why));
    }
    free(program);
}

/* ---- the bus ---- */

void model_on_line(void *ctx, json line)
{
    model *m = ctx;
    json type = json_get(line, "type");
    model_replay(m, line);
    if (json_eq(type, "run_start"))
    {
        m->run = (int)json_get_num(json_get(line, "run"), m->run + 1);
        m->turn = m->block_retries = 0;
    }
    else if (json_eq(type, "continue") && !m->turn_open)
        begin_turn(m);
    else if ((json_eq(type, "preempt") || json_eq(type, "abort")) && m->turn_open && !m->stopping)
    {
        m->stopping = true;
        proc_term(m->provider);
    }
    else if (json_eq(type, "replace") && m->retrying)
    {
        m->retrying = false; /* the replace is in the session: the request goes again */
        if (m->stopping)
            stopped(m);
        else
            begin_turn(m);
    }
}

void model_put_status(void *ctx, json_list *members)
{
    model *m = ctx;
    json_printf(members, "%r'model':%s", members->n ? "," : "", m->config->provider_model);
}
