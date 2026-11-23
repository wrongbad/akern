/* inbox: the sequencer, the waiting room in front of the conversation (akern.md §3, delivery
   modes), and the one actor that decides what happens next. Owns the items queued by delivery
   mode (sends from stdin and finished jobs' results); a slot per tool call of the turn, filled by
   its block line, which, unfilled when the results go out, stays as the job's until its late
   result arrives; append commands' entries, held from a reset on until a turn boundary; deciding
   when what is due goes into one user message; runs, from run_start to run_end; the orders it
   gives: continue (the model's turn), preempt and abort (the reply stops, the calls are killed);
   handing queued prompts back on an abort; checking and replying to send, abort and append; and
   when the kernel is done: settled, then exit once stdin has ended (with -p, once it has run). It
   learns what a reply or a call is doing only from the lines on the bus.
   delivery mode = the point at which a queued item may enter the conversation;
   run = from a delivery while idle to run_end; quiescent = no run, nothing due, no job. */

#include "akern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Delivery modes (akern.md §3), as bits: an item is due when its mode is in the mask. */
enum { NOW = 1, TURN = 2, IDLE = 4, DEFERRED = 8, DUE = NOW | TURN | IDLE };

static const char *const when_names[] = {"now", "turn", "idle", "deferred"};

typedef struct item {
    char id[NEW_ID_CHARS + 1];
    const char *source; /* prompt, inject or job */
    unsigned when;
    size_t seq, nblocks;
    json_buf content; /* a string, or an array of blocks */
} item;

/* A tool call of the turn, filled with its tool_result when it ends. One still empty when the
   results go out stays, a job's, until its late result arrives. */
typedef struct slot {
    char *call;
    bool of_turn, blocking;
    json_buf block;
} slot;

static const char *when_name(unsigned when)
{
    size_t i = 0;
    while (when >>= 1)
        i++;
    return when_names[i];
}

/* A mode by name; TURN when absent, 0 when unknown. */
static unsigned parse_when(json name)
{
    for (size_t i = 0; name.p && i < LEN(when_names); i++)
        if (json_eq(name, when_names[i]))
            return 1u << i;
    return name.p ? 0 : TURN;
}

/* ---- items ---- */

/* Adds content to a list of blocks: a string as a text block (empty: "(empty)"), an array's
   blocks as they are. */
static void add_content(json_list *blocks, json content)
{
    content = json_trim(content);
    if (json_is(content, '"'))
    {
        buf text = {0};
        json_decode(content, &text);
        json_printf(
            blocks,
            "%r{'type':'text','text':%S}",
            blocks->n ? "," : "",
            text.n ? buf_span(&text) : S("(empty)")
        );
        buf_free(&text);
        return;
    }
    jiter it = json_iter(content);
    for (json block; json_next(&it, NULL, &block);)
        json_printf(blocks, "%r%R", blocks->n ? "," : "", block);
}

static void queue(inbox *in, const char *id, const char *source, unsigned when, json content)
{
    item it = {.source = source, .when = when, .seq = in->nadded++};
    snprintf(it.id, sizeof it.id, "%s", id);
    content = json_trim(content);
    jiter blocks = json_iter(content);
    for (json b; json_next(&blocks, NULL, &b);)
        it.nblocks++;
    it.nblocks = json_is(content, '"') ? 1 : it.nblocks;
    buf_set(&it.content, content);
    VEC_PUSH(in->items, in->nitems, in->capitems, it);
    bus_printf(
        in->bus,
        "{'type':'queued','item':%s,'source':%s,'when':%s,'n':%d}",
        it.id,
        source,
        when_name(when),
        (int)in->nitems
    );
}

static bool is_due(const inbox *in, unsigned mask, size_t from_seq)
{
    for (size_t i = 0; i < in->nitems; i++)
        if (in->items[i].seq >= from_seq && (in->items[i].when & mask))
            return true;
    return false;
}

static bool has_turn_slots(const inbox *in)
{
    for (size_t i = 0; i < in->nslots; i++)
        if (in->slots[i].of_turn)
            return true;
    return false;
}

static bool is_turn_filled(const inbox *in)
{
    for (size_t i = 0; i < in->nslots; i++)
        if (in->slots[i].of_turn && in->slots[i].blocking && !in->slots[i].block.n)
            return false;
    return true;
}

static slot *find_slot(const inbox *in, json call)
{
    for (size_t i = 0; i < in->nslots; i++)
        if (json_eq(call, in->slots[i].call))
            return &in->slots[i];
    return NULL;
}

/* A call's tool_result: into its slot of the turn; else a job's, queued. */
static void fill(inbox *in, json block)
{
    slot *s = find_slot(in, json_get(block, "tool_use_id"));
    if (s && s->of_turn)
    {
        buf_set(&s->block, block);
        return;
    }
    if (s)
    {
        free(s->call), buf_free(&s->block);
        memmove(s, s + 1, (size_t)(in->slots + --in->nslots - s) * sizeof *s);
    }
    char id[NEW_ID_CHARS + 1];
    json_buf content = {0};
    bus_new_id(in->bus, id);
    json_printf(&content, "[%R]", block);
    queue(in, id, "job", TURN, buf_span(&content));
    buf_free(&content);
}

/* ---- delivering ---- */

/* One user message: with_results, the turn's results first, in call order; then the items due
   in the mask and, with any mask, deferred ones, in arrival order, each named in meta.items with
   where its blocks are. With the results, a pending line names the turn's calls that are jobs
   from now on: the turn is answered even when no message goes out. True if either went out. */
static bool deliver(inbox *in, unsigned mask, bool with_results)
{
    json_list blocks = {0}, items = {0}, pending = {0};
    size_t n = 0, kept = 0;
    for (size_t i = 0; i < in->nslots; i++)
    {
        slot *s = &in->slots[i];
        if (with_results && s->of_turn && s->block.n)
        {
            json_printf(&blocks, "%r%R", n++ ? "," : "", buf_span(&s->block));
            free(s->call), buf_free(&s->block);
            continue;
        }
        if (with_results && s->of_turn)
            json_printf(&pending, "%r%s", pending.n ? "," : "", s->call),
                s->of_turn = s->blocking = false;
        in->slots[kept++] = *s;
    }
    in->nslots = kept;
    size_t results = n;
    kept = 0;
    for (size_t i = 0; i < in->nitems; i++)
    {
        item *it = &in->items[i];
        if (!(it->when & (mask ? mask | DEFERRED : 0)))
        {
            in->items[kept++] = *it;
            continue;
        }
        json_printf(
            &items,
            "%r{'id':%s,'source':%s,'at':%D,'blocks':%D}",
            items.n ? "," : "",
            it->id,
            it->source,
            (long long)n,
            (long long)it->nblocks
        );
        add_content(&blocks, buf_span(&it->content));
        n += it->nblocks;
        buf_free(&it->content);
    }
    in->nitems = kept;
    if (blocks.n)
    {
        char id[NEW_ID_CHARS + 1];
        json_buf message = {0}, meta = {0};
        bus_new_id(in->bus, id);
        json_printf(&message, "{'role':'user','content':[%R]}", buf_span(&blocks));
        json_printf(
            &meta,
            "{'run':%d,'source':%s",
            in->run,
            !results  ? "inbox"
            : items.n ? "results+inbox"
                      : "results"
        );
        if (items.n)
            json_printf(&meta, ",'items':[%R]", buf_span(&items));
        buf_putc(&meta, '}');
        bus_printf(
            in->bus,
            "{'type':'message','id':%s,'ts':%D,'message':%R,'meta':%R}",
            id,
            get_unix_seconds(),
            buf_span(&message),
            buf_span(&meta)
        );
        buf_free(&message), buf_free(&meta);
    }
    if (pending.n)
        bus_printf(
            in->bus,
            "{'type':'pending','run':%d,'calls':[%R]}",
            in->run,
            buf_span(&pending)
        );
    bool any = blocks.n || pending.n;
    buf_free(&blocks), buf_free(&items), buf_free(&pending);
    return any;
}

/* The model's turn: what was just delivered gets a reply. */
static void give_turn(inbox *in)
{
    in->turn_open = true;
    bus_tellf(in->bus, "{'type':'continue','run':%d}", in->run);
}

/* ---- entries ---- */

/* An append command's entry, with an id and a ts; then its ok. */
static void put_entry(inbox *in, json command)
{
    json entry = json_get(command, "entry"), ref = json_get(command, "ref");
    char id[NEW_ID_CHARS + 1];
    json_buf line = {0};
    bus_new_id(in->bus, id);
    json_printf(
        &line,
        "{'type':%R,'id':%s,'ts':%D",
        json_get(entry, "type"),
        id,
        get_unix_seconds()
    );
    jiter it = json_iter(entry);
    for (json k, v; json_next(&it, &k, &v);)
        if (!json_eq(k, "type") && !json_eq(k, "id") && !json_eq(k, "ts"))
            json_printf(&line, ",%R:%R", k, v);
    buf_putc(&line, '}');
    bus_put(in->bus, buf_span(&line));
    if (ref.p)
        bus_printf(in->bus, "{'type':'ok','id':%s,'ref':%R}", id, ref);
    buf_free(&line);
}

/* At a turn boundary: the entries held since a reset. */
static bool put_held_entries(inbox *in)
{
    for (size_t i = 0; i < in->nheld; i++)
    {
        put_entry(in, buf_span(&in->held[i]));
        buf_free(&in->held[i]);
    }
    bool any = in->nheld > 0;
    in->nheld = 0;
    return any;
}

/* ---- runs ---- */

static void start_run(inbox *in)
{
    in->run++;
    in->turn = 0;
    in->run_open = true;
    in->preempting = in->turn_open = in->turn_failed = false;
    in->turn_error.n = 0;
    bus_printf(in->bus, "{'type':'run_start','run':%d}", in->run);
}

static void end_run(inbox *in, const char *reason)
{
    bool failed = !strcmp(reason, "error");
    bus_printf(
        in->bus,
        "{'type':'run_end','run':%d,'reason':%s,'returned':[%r],'items':[%r],'error':%R}",
        in->run,
        reason,
        buf_cstr(&in->returned_contents),
        buf_cstr(&in->returned_ids),
        failed && in->turn_error.n ? buf_span(&in->turn_error) : NOSPAN
    );
    in->last_run_failed = failed;
    in->returned_contents.n = in->returned_ids.n = 0;
    in->run_open = in->preempting = false;
}

/* An abort line; the queued turn and idle prompts handed back (in a returned line when no run is
   open, else in run_end); queued job results deferred; the open run ends once the reply has
   stopped and the killed calls have answered. */
static void abort_run(inbox *in, bool kill_jobs)
{
    bus_printf(in->bus, "{'type':'abort','jobs':%b}", kill_jobs);
    size_t kept = 0;
    for (size_t i = 0; i < in->nitems; i++)
    {
        item *it = &in->items[i];
        bool job = !strcmp(it->source, "job");
        it->when = job ? DEFERRED : it->when;
        if (job || !(it->when & (TURN | IDLE)))
        {
            in->items[kept++] = *it;
            continue;
        }
        json_printf(
            &in->returned_contents,
            "%r%R",
            in->returned_contents.n ? "," : "",
            buf_span(&it->content)
        );
        json_printf(&in->returned_ids, "%r%s", in->returned_ids.n ? "," : "", it->id);
        buf_free(&it->content);
    }
    in->nitems = kept;
    if (in->run_open)
        in->abort_pending = true; /* the rest goes in run_end */
    else if (in->returned_ids.n)
        bus_printf(
            in->bus,
            "{'type':'returned','returned':[%R],'items':[%R]}",
            buf_span(&in->returned_contents),
            buf_span(&in->returned_ids)
        );
    if (!in->run_open)
        in->returned_contents.n = in->returned_ids.n = 0;
}

/* ---- commands ---- */

static void reply_ok(inbox *in, json ref, const char *item)
{
    if (ref.p && item)
        bus_printf(in->bus, "{'type':'ok','item':%s,'ref':%R}", item, ref);
    else if (ref.p)
        bus_printf(in->bus, "{'type':'ok','ref':%R}", ref);
}

static void reply_error(inbox *in, json ref, const char *message)
{
    if (ref.p)
        bus_printf(
            in->bus,
            "{'type':'error','where':'command','message':%s,'ref':%R}",
            message,
            ref
        );
    else
        bus_printf(in->bus, "{'type':'error','where':'command','message':%s}", message);
}

/* send, abort and append, from stdin (akern.md §3). */
static void on_command(inbox *in, json command)
{
    json type = json_get(command, "type"), ref = json_get(command, "ref"),
         entry = json_get(command, "entry");
    json content = json_get(command, "content"), source = json_get(command, "source");
    unsigned when = parse_when(json_get(command, "when"));
    if (json_eq(type, "status"))
        return; /* input's */
    if (json_eq(type, "abort"))
    {
        abort_run(in, json_is_true(json_get(command, "jobs")));
        reply_ok(in, ref, NULL);
    }
    else if (json_eq(type, "append") && !json_is(json_get(entry, "type"), '"'))
        reply_error(in, ref, "entry needs a type");
    else if (json_eq(type, "append") && (in->nheld || json_eq(json_get(entry, "type"), "reset")))
    {
        VEC_PUSH(in->held, in->nheld, in->capheld, (buf){0});
        buf_set(&in->held[in->nheld - 1], command); /* until a turn boundary */
    }
    else if (json_eq(type, "append"))
        put_entry(in, command);
    else if (!when)
        reply_error(in, ref, "when must be now, turn, idle or deferred");
    else if (!json_is_valid(content) || !(json_is(content, '"') || json_is(content, '[')))
        reply_error(in, ref, "content must be a string or an array of content blocks");
    else if (source.p && !json_eq(source, "prompt") && !json_eq(source, "inject"))
        reply_error(in, ref, "source must be prompt or inject");
    else
    {
        char id[NEW_ID_CHARS + 1];
        bus_new_id(in->bus, id);
        reply_ok(in, ref, id);
        queue(in, id, json_eq(source, "inject") ? "inject" : "prompt", when, content);
    }
}

/* ---- deciding ---- */

/* One decision from the state the lines so far left; true if it moved. Each action updates the
   state at once, so the next decision never repeats it. */
static bool step(inbox *in)
{
    if (in->abort_pending)
    {
        if (in->turn_open || !is_turn_filled(in))
            return false; /* the reply stops, and the killed calls answer, on their own */
        in->abort_pending = false;
        if (in->run_open && has_turn_slots(in))
            deliver(in, 0, true);
        if (in->run_open)
            end_run(in, "aborted");
        return true;
    }
    if (!in->run_open)
    {
        bool moved = !in->stopping && put_held_entries(in);
        if (in->stopping || !is_due(in, DUE, 0))
            return moved;
        start_run(in);
    }
    else if (in->turn_open)
    {
        if (in->preempting || !is_due(in, NOW, 0))
            return false;
        deliver(in, NOW | TURN, false);
        bus_printf(in->bus, "{'type':'preempt','run':%d,'turn':%d}", in->run, in->turn);
        in->preempting = true; /* the turn ends, then the model answers this message */
        return true;
    }
    else if (has_turn_slots(in))
    {
        if (!is_turn_filled(in) && !is_due(in, NOW, 0) && !is_due(in, TURN, in->mark))
            return false;
        if (deliver(in, NOW | TURN, true))
            give_turn(in);
        return true;
    }
    else
    {
        put_held_entries(in);
        if (!is_due(in, DUE, 0))
        {
            end_run(in, in->turn_failed ? "error" : "done");
            return true;
        }
    }
    if (deliver(in, DUE, false))
        give_turn(in);
    return true;
}

/* A settled line each time the kernel becomes quiescent after working; exit once it is done. */
static void note_settling(inbox *in)
{
    bool quiescent = !in->run_open && !in->abort_pending && !is_due(in, DUE, 0) && !in->nslots;
    if (quiescent && !in->settled && !in->stopping)
        bus_printf(in->bus, "{'type':'settled','run':%d}", in->run);
    in->settled = quiescent;
    if (quiescent && !in->stopping && (in->eof || (in->config->print_answers && in->run)))
    {
        in->stopping = true;
        bus_tellf(in->bus, "{'type':'exit','code':%d}", in->last_run_failed ? 1 : 0);
    }
}

/* ---- the bus ---- */

void inbox_on_line(void *ctx, json line)
{
    inbox *in = ctx;
    json type = json_get(line, "type"), message = json_get(line, "message");
    if (json_eq(type, "decide"))
    {
        in->deciding = false;
        while (step(in))
            ;
        note_settling(in);
        return;
    }
    if (json_eq(type, "command"))
        on_command(in, json_get(line, "command"));
    else if (json_eq(type, "eof"))
        in->eof = true;
    else if (json_eq(type, "exit") && !in->stopping)
    {
        in->stopping = true;
        if (in->run_open)
            abort_run(in, true);
    }
    else if (json_eq(type, "close"))
    {
        /* every call answered: one last user message with the jobs' results */
        for (size_t i = 0; i < in->nitems; i++)
            in->items[i].when = strcmp(in->items[i].source, "job") ? 0 : DEFERRED;
        deliver(in, DEFERRED, false);
    }
    else if (json_eq(type, "turn_start"))
    {
        in->turn = (int)json_get_num(json_get(line, "turn"), in->turn);
        buf text = {0};
        json_decode(json_get(line, "id"), &text);
        snprintf(in->reply_id, sizeof in->reply_id, "%s", buf_cstr(&text));
        buf_free(&text);
    }
    else if (json_eq(type, "turn_end"))
    {
        in->turn_open = false;
        in->turn_failed = json_eq(json_get(line, "reason"), "error");
        buf_set(
            &in->turn_error,
            json_is(json_get(line, "error"), '{') ? json_get(line, "error") : NOSPAN
        );
        if (in->preempting && !in->abort_pending && !in->stopping)
            give_turn(in); /* a message delivered while the reply streamed is still owed one */
        in->preempting = false;
    }
    else if (
        json_eq(type, "message") && json_eq(json_get(message, "role"), "assistant") &&
        json_eq(json_get(line, "id"), in->reply_id)
    )
    {
        in->mark = in->nadded; /* a turn item from now on cuts the wait for its calls */
        jiter it = json_iter(json_get(message, "content"));
        for (
            json b; json_eq(json_get(json_get(line, "meta"), "stop"), "tool_use") &&
                    json_next(&it, NULL, &b);
        )
            if (json_eq(json_get(b, "type"), "tool_use"))
            {
                slot s =
                    {.call = json_decode_dup(json_get(b, "id")), .of_turn = true, .blocking = true};
                s.call = s.call ? s.call : xstrdup("");
                VEC_PUSH(in->slots, in->nslots, in->capslots, s);
            }
    }
    else if (json_eq(type, "block"))
        fill(in, json_get(line, "block"));
    else if (json_eq(type, "released"))
    {
        slot *s = find_slot(in, json_get(line, "call"));
        if (s)
            s->blocking = false;
    }
    else
        return; /* nothing that moves the inbox */
    /* Decide once what is queued now has been seen: the lines of one wake-up (a send and an
       abort in one write, a job's result and its waiter's) are taken together. */
    if (!in->deciding)
    {
        in->deciding = true;
        bus_tellf(in->bus, "{'type':'decide'}");
    }
}

void inbox_put_status(void *ctx, json_list *members)
{
    inbox *in = ctx;
    const char *state = !in->run_open                                            ? "idle"
                        : in->turn_open && (in->abort_pending || in->preempting) ? "aborting"
                        : in->turn_open                                          ? "stream"
                        : has_turn_slots(in)                                     ? "waiting"
                                                                                 : "stream";
    json_printf(
        members,
        "%r'state':%s,'run':%d,'turn':%d,'inbox':%d",
        members->n ? "," : "",
        state,
        in->run,
        in->turn,
        (int)in->nitems
    );
}
