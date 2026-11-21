#include "session.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "loop.h"

enum { NEW_ID_CHARS = 8, FIRST_INDEX_SLOTS = 1024 };

typedef enum { OTHER_ENTRY, MESSAGE_ENTRY, SETTINGS_ENTRY } entry_kind;

typedef struct entry {
    char id[MAX_ID_CHARS + 1], parent_id[MAX_ID_CHARS + 1];
    off_t offset;
    size_t length;
    entry_kind kind;
    long replacement;
} entry;

static uint64_t hash(const char *id)
{
    uint64_t h = 1469598103934665603ULL;
    for (; *id; id++)
        h = (h ^ (unsigned char)*id) * 1099511628211ULL;
    return h;
}

static void slot_put(session *s, size_t entry_index)
{
    size_t k = hash(s->entries[entry_index].id) & (s->nslots - 1);
    while (s->slots[k])
        k = (k + 1) & (s->nslots - 1);
    s->slots[k] = entry_index + 1;
}

static void grow_index(session *s)
{
    if (s->nentries * 2 < s->nslots)
        return;
    free(s->slots);
    s->nslots = s->nslots ? s->nslots * 2 : FIRST_INDEX_SLOTS;
    s->slots = xcalloc(s->nslots, sizeof *s->slots);
    for (size_t i = 0; i < s->nentries; i++)
        slot_put(s, i);
}

static long find_entry(const session *s, const char *id)
{
    if (!*id || !s->nslots)
        return -1;
    for (size_t k = hash(id) & (s->nslots - 1); s->slots[k]; k = (k + 1) & (s->nslots - 1))
        if (strcmp(s->entries[s->slots[k] - 1].id, id) == 0)
            return (long)s->slots[k] - 1;
    return -1;
}

static void copy_id(char dst[MAX_ID_CHARS + 1], json id)
{
    buf text = {0};
    dst[0] = 0;
    if (json_decode(id, &text) && text.n <= MAX_ID_CHARS)
        memcpy(dst, text.p, text.n + 1);
    buf_free(&text);
}

static void index_line(session *s, json line, off_t offset)
{
    json type = json_get(line, "type"), id = json_get(line, "id");
    if (offset == 0 || !json_is(id, '"'))
        return;
    entry e = {
        .offset = offset,
        .length = line.n,
        .kind = json_eq(type, "message")    ? MESSAGE_ENTRY
                : json_eq(type, "settings") ? SETTINGS_ENTRY
                                            : OTHER_ENTRY,
        .replacement = -1
    };
    char target_id[MAX_ID_CHARS + 1];
    copy_id(e.id, id);
    copy_id(e.parent_id, json_get(line, "parent"));
    if (!e.id[0])
        return;
    VEC_PUSH(s->entries, s->nentries, s->capentries, e);
    grow_index(s);
    slot_put(s, s->nentries - 1);
    strcpy(s->head_id, e.id);
    copy_id(target_id, json_get(line, "target"));
    long target = json_eq(type, "replace") && json_is(json_get(line, "message"), '{')
                      ? find_entry(s, target_id)
                      : -1;
    if (target >= 0)
        s->entries[target].replacement = (long)s->nentries - 1;
}

/* Indexes every entry, reading the file in place through a read-only mapping. */
static void index_file(session *s)
{
    struct stat st;
    if (fstat(s->fd, &st) < 0)
        die("%s: %s", s->config->session_path, strerror(errno));
    s->file_size = st.st_size;
    if (st.st_size == 0)
        return;
    size_t size = (size_t)st.st_size;
    const char *file = mmap(NULL, size, PROT_READ, MAP_PRIVATE, s->fd, 0);
    if (file == MAP_FAILED)
        die("%s: %s", s->config->session_path, strerror(errno));
    for (size_t offset = 0; offset < size;)
    {
        const char *nl = memchr(file + offset, '\n', size - offset);
        size_t len = nl ? (size_t)(nl - (file + offset)) : size - offset;
        span line = {file + offset, len};
        if (json_is_valid(line))
            index_line(s, line, (off_t)offset);
        offset += len + 1;
    }
    s->ends_mid_line = file[size - 1] != '\n';
    munmap((void *)file, size);
}

static void new_id(session *s, char id[NEW_ID_CHARS + 1])
{
    do
    {
        s->id_rng ^= s->id_rng << 13;
        s->id_rng ^= s->id_rng >> 7;
        s->id_rng ^= s->id_rng << 17;
        snprintf(id, NEW_ID_CHARS + 1, "%08x", (unsigned)(s->id_rng >> 16));
    } while (find_entry(s, id) >= 0);
}

static void lock_owner(session *s)
{
    buf path = {0};
    buf_printf(&path, "%s.owner", s->config->session_path);
    s->owner_lock_fd = open(buf_cstr(&path), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    struct flock lk = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    if (s->owner_lock_fd < 0)
        die("%s: %s", path.p, strerror(errno));
    if (fcntl(s->owner_lock_fd, F_SETLK, &lk) < 0)
    {
        char owner_json[256] = {0};
        pread(s->owner_lock_fd, owner_json, sizeof owner_json - 1, 0);
        die("session %s is owned by another kernel (pid %d)",
            s->config->session_path,
            (int)json_get_num(json_get(S(owner_json), "pid"), 0));
    }
    json_buf owner = {0};
    json_printf(&owner, "{'pid':%d}\n", (int)getpid());
    ftruncate(s->owner_lock_fd, 0);
    pwrite(s->owner_lock_fd, owner.p, owner.n, 0);
    buf_free(&owner);
    buf_free(&path);
}

static void write_header(session *s)
{
    char id[NEW_ID_CHARS + 1], cwd[4096];
    json_buf line = {0};
    new_id(s, id);
    json_printf(
        &line,
        "{'type':'session','v':1,'id':%s,'cwd':%s,'created':%D}\n",
        id,
        getcwd(cwd, sizeof cwd) ? cwd : "",
        get_unix_seconds()
    );
    if (!write_all(s->fd, buf_span(&line)))
        die("%s: %s", s->config->session_path, strerror(errno));
    s->file_size += (off_t)line.n;
    buf_free(&line);
}

void session_open(session *s)
{
    s->id_rng = (uint64_t)get_unix_seconds() ^ ((uint64_t)getpid() << 32) ^
                (uint64_t)(get_monotonic_seconds() * 1e9);
    s->fd = open(s->config->session_path, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (s->fd < 0)
        die("%s: %s", s->config->session_path, strerror(errno));
    lock_owner(s);
    index_file(s);
    if (s->file_size == 0)
        write_header(s);
}

void session_close(session *s)
{
    if (s->owner_lock_fd >= 0)
        ftruncate(s->owner_lock_fd, 0);
}

const char *session_get_head_id(const session *s)
{
    return s->head_id;
}
bool session_has_failed(const session *s)
{
    return s->failed;
}

bool session_has_entry(const session *s, const char *id)
{
    return find_entry(s, id) >= 0;
}

const char *session_append(
    session *s,
    const char *type,
    const char *parent_id,
    const json_list *members
)
{
    char id[NEW_ID_CHARS + 1];
    if (s->failed)
        return NULL;
    new_id(s, id);
    const char *parent = parent_id == AT_HEAD ? s->head_id : parent_id;
    json_buf line = {0};
    size_t start = s->ends_mid_line ? 1 : 0; /* a torn last line: begin a new one */
    if (start)
        buf_putc(&line, '\n');
    json_printf(
        &line,
        "{'type':%s,'id':%s,'parent':%s,'ts':%D",
        type,
        id,
        *parent ? parent : NULL,
        get_unix_seconds()
    );
    if (members->n)
        buf_putc(&line, ',');
    buf_putspan(&line, buf_span(members));
    buf_puts(&line, "}\n");
    s->failed = !write_all(s->fd, buf_span(&line));
    if (!s->failed)
    {
        json entry = {line.p + start, line.n - start - 1};
        index_line(s, entry, s->file_size + (off_t)start);
        s->file_size += (off_t)line.n;
        s->ends_mid_line = false;
    }
    buf_free(&line);
    return s->failed ? NULL : s->head_id;
}

static void read_line_of(const session *s, const entry *e, json_buf *line)
{
    line->p = xmalloc(e->length + 1);
    line->cap = e->length + 1;
    line->n = pread(s->fd, line->p, e->length, e->offset) == (ssize_t)e->length ? e->length : 0;
}

/* The indexes of the entries of one kind on the chain from the head back to a root, newest
   first; returns how many. */
static size_t collect_chain(const session *s, entry_kind kind, size_t **lineage)
{
    size_t n = 0, cap = 0, steps = 0;
    for (
        long i = find_entry(s, s->head_id); i >= 0 && steps++ < s->nentries;
        i = find_entry(s, s->entries[i].parent_id)
    )
        if (s->entries[i].kind == kind)
            VEC_PUSH(*lineage, n, cap, (size_t)i);
    return n;
}

static bool put_message_of(const session *s, json_buf *out, const char *separator, const entry *e)
{
    if (e->replacement >= 0)
        e = &s->entries[e->replacement];
    json_buf line = {0};
    read_line_of(s, e, &line);
    json message = json_get(buf_span(&line), "message");
    bool found = message.p;
    if (found)
        buf_puts(out, separator), buf_putspan(out, message);
    buf_free(&line);
    return found;
}

bool session_read_message(const session *s, const char *id, json_buf *message)
{
    long i = find_entry(s, id);
    return i >= 0 && s->entries[i].kind == MESSAGE_ENTRY &&
           put_message_of(s, message, "", &s->entries[i]);
}

void session_read_context(const session *s, json_buf *messages, strv *ids)
{
    size_t *lineage = NULL;
    size_t n = collect_chain(s, MESSAGE_ENTRY, &lineage);
    buf_putc(messages, '[');
    while (n--)
        if (put_message_of(s, messages, ids->n ? "," : "", &s->entries[lineage[n]]))
            strv_push(ids, xstrdup(s->entries[lineage[n]].id));
    buf_putc(messages, ']');
    free(lineage);
}

void session_read_settings(const session *s, json_list *entries)
{
    size_t *lineage = NULL;
    size_t n = collect_chain(s, SETTINGS_ENTRY, &lineage);
    while (n--)
    {
        json_buf line = {0};
        read_line_of(s, &s->entries[lineage[n]], &line);
        if (json_is_valid(buf_span(&line)))
        {
            if (entries->n)
                buf_putc(entries, ',');
            buf_putspan(entries, json_trim(buf_span(&line)));
        }
        buf_free(&line);
    }
    free(lineage);
}
