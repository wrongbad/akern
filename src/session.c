#include "session.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { NEW_ID_CHARS = 8, FIRST_INDEX_SLOTS = 1024 };

typedef struct entry {
    char   id[MAX_ID_CHARS + 1], parent_id[MAX_ID_CHARS + 1];
    off_t  offset;
    size_t length;
    bool   is_message;
} entry;

static uint64_t hash(const char *id)
{
    uint64_t h = 1469598103934665603ULL;
    for (; *id; id++) h = (h ^ (unsigned char)*id) * 1099511628211ULL;
    return h;
}

static void slot_put(session *s, size_t entry_index)
{
    size_t k = hash(s->entries[entry_index].id) & (s->nslots - 1);
    while (s->slots[k]) k = (k + 1) & (s->nslots - 1);
    s->slots[k] = entry_index + 1;
}

static void grow_index(session *s)
{
    if (s->nentries * 2 < s->nslots) return;
    free(s->slots);
    s->nslots = s->nslots ? s->nslots * 2 : FIRST_INDEX_SLOTS;
    s->slots = xcalloc(s->nslots, sizeof *s->slots);
    for (size_t i = 0; i < s->nentries; i++) slot_put(s, i);
}

static long find_entry(const session *s, const char *id)
{
    if (!*id || !s->nslots) return -1;
    for (size_t k = hash(id) & (s->nslots - 1); s->slots[k]; k = (k + 1) & (s->nslots - 1))
        if (strcmp(s->entries[s->slots[k] - 1].id, id) == 0) return (long)s->slots[k] - 1;
    return -1;
}

static void copy_id(char dst[MAX_ID_CHARS + 1], json id)
{
    buf text = { 0 };
    dst[0] = 0;
    if (json_text(id, &text) && text.n <= MAX_ID_CHARS) memcpy(dst, text.p, text.n + 1);
    buf_free(&text);
}

static void index_line(session *s, json line, off_t offset)
{
    json type = json_get(line, "type"), id = json_get(line, "id");
    if (offset == 0 || !json_is(id, '"')) return;
    entry e = { .offset = offset, .length = line.n, .is_message = json_eq(type, "message") };
    copy_id(e.id, id);
    copy_id(e.parent_id, json_get(line, "parent"));
    if (!e.id[0]) return;
    VEC_PUSH(s->entries, s->nentries, s->capentries, e);
    grow_index(s);
    slot_put(s, s->nentries - 1);
    strcpy(s->head_id, e.id);
}

static void index_file(session *s)
{
    buf all = { 0 };
    char chunk[65536];
    ssize_t r;
    while ((r = pread(s->fd, chunk, sizeof chunk, (off_t)all.n)) > 0) buf_put(&all, chunk, (size_t)r);
    for (size_t offset = 0; offset < all.n;) {
        char *nl = memchr(all.p + offset, '\n', all.n - offset);
        size_t len = nl ? (size_t)(nl - (all.p + offset)) : all.n - offset;
        span line = { all.p + offset, len };
        if (json_valid(line)) index_line(s, line, (off_t)offset);
        offset += len + 1;
    }
    s->file_size = (off_t)all.n;
    buf_free(&all);
}

static void new_id(session *s, char id[NEW_ID_CHARS + 1])
{
    do {
        s->id_rng ^= s->id_rng << 13;
        s->id_rng ^= s->id_rng >> 7;
        s->id_rng ^= s->id_rng << 17;
        snprintf(id, NEW_ID_CHARS + 1, "%08x", (unsigned)(s->id_rng >> 16));
    } while (find_entry(s, id) >= 0);
}

static void lock_owner(session *s)
{
    buf path = { 0 };
    buf_printf(&path, "%s.owner", s->config->session_path);
    s->owner_lock_fd = open(buf_cstr(&path), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    struct flock lk = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    if (s->owner_lock_fd < 0) die("%s: %s", path.p, strerror(errno));
    if (fcntl(s->owner_lock_fd, F_SETLK, &lk) < 0) {
        char owner_json[256] = { 0 };
        pread(s->owner_lock_fd, owner_json, sizeof owner_json - 1, 0);
        die("session %s is owned by another kernel (pid %d)", s->config->session_path,
            (int)json_num(json_get(S(owner_json), "pid"), 0));
    }
    json_buf owner = { 0 };
    json_printf(&owner, "{'pid':%d}\n", (int)getpid());
    ftruncate(s->owner_lock_fd, 0);
    pwrite(s->owner_lock_fd, owner.p, owner.n, 0);
    buf_free(&owner);
    buf_free(&path);
}

static void write_header(session *s)
{
    char id[NEW_ID_CHARS + 1], cwd[4096];
    json_buf line = { 0 };
    new_id(s, id);
    json_printf(&line, "{'type':'session','v':1,'id':%s,'cwd':%s,'created':%D}\n",
                id, getcwd(cwd, sizeof cwd) ? cwd : "", unix_seconds());
    if (!write_all(s->fd, buf_span(&line))) die("%s: %s", s->config->session_path, strerror(errno));
    s->file_size += (off_t)line.n;
    buf_free(&line);
}

void session_open(session *s)
{
    s->id_rng = (uint64_t)unix_seconds() ^ ((uint64_t)getpid() << 32) ^ (uint64_t)(monotonic_seconds() * 1e9);
    s->fd = open(s->config->session_path, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (s->fd < 0) die("%s: %s", s->config->session_path, strerror(errno));
    lock_owner(s);
    index_file(s);
    if (s->file_size == 0) write_header(s);
}

void session_close(session *s)
{
    if (s->owner_lock_fd >= 0) ftruncate(s->owner_lock_fd, 0);
}

const char *session_head_id(const session *s) { return s->head_id; }
bool        session_failed(const session *s)  { return s->failed; }

bool session_has_entry(const session *s, const char *id)
{
    return find_entry(s, id) >= 0;
}

const char *session_append(session *s, const char *type, const char *parent_id, const json_list *members)
{
    char id[NEW_ID_CHARS + 1];
    if (s->failed) return NULL;
    new_id(s, id);
    const char *parent = parent_id == AT_HEAD ? s->head_id : parent_id;
    json_buf line = { 0 };
    json_printf(&line, "{'type':%s,'id':%s,'parent':%s,'ts':%D", type, id, *parent ? parent : NULL, unix_seconds());
    if (members->n) buf_putc(&line, ',');
    buf_putspan(&line, buf_span(members));
    buf_puts(&line, "}\n");
    s->failed = !write_all(s->fd, buf_span(&line));
    if (!s->failed) index_line(s, (json){ line.p, line.n - 1 }, s->file_size), s->file_size += (off_t)line.n;
    buf_free(&line);
    return s->failed ? NULL : s->head_id;
}

static void put_message_of(const session *s, json_buf *messages, const entry *e)
{
    json_buf line = { 0 };
    line.p = xmalloc(e->length + 1);
    line.cap = e->length + 1;
    line.n = pread(s->fd, line.p, e->length, e->offset) == (ssize_t)e->length ? e->length : 0;
    json message = json_get(buf_span(&line), "message");
    if (message.p) {
        if (messages->n > 1) buf_putc(messages, ',');
        buf_putspan(messages, message);
    }
    buf_free(&line);
}

void session_context(const session *s, json_buf *messages)
{
    size_t *lineage = NULL, n = 0, cap = 0;
    size_t steps = 0;
    for (long i = find_entry(s, s->head_id); i >= 0 && steps++ < s->nentries; i = find_entry(s, s->entries[i].parent_id))
        if (s->entries[i].is_message) VEC_PUSH(lineage, n, cap, (size_t)i);
    buf_putc(messages, '[');
    while (n--) put_message_of(s, messages, &s->entries[lineage[n]]);
    buf_putc(messages, ']');
    free(lineage);
}
