/* session: the session log (protocols.md §2), which this kernel alone appends to while it
   runs. Owns the ownership lease, appending entries of any type, the head, building the
   context, and paths for files that belong to the session (<session>.d/). What to append, and
   when, is decided by the driver and ctl.
   head = the last entry in the file; context = the messages on the parent chain from the head
   back to a root. */

#ifndef SESSION_H
#define SESSION_H

#include <stdint.h>
#include <sys/types.h>

#include "main.h"

#define AT_HEAD NULL
#define AS_ROOT ""

enum { MAX_ID_CHARS = 32 };

typedef struct session {
    char          *path;
    int            fd, owner_lock_fd;
    off_t          file_size;
    struct entry  *entries;
    size_t         nentries, capentries;
    size_t        *slots, nslots;
    uint64_t       id_rng;
    char           head_id[MAX_ID_CHARS + 1];
    const config  *config;
    struct sink   *events;
    struct loop   *loop;
} session;

void        session_open(session *s);
void        session_close(session *s);
const char *session_path(const session *s);
const char *session_head_id(const session *s);
const char *session_append(session *s, const char *type, const char *parent_id, const json_list *members);
bool        session_has_entry(const session *s, const char *id);
void        session_context(const session *s, json_buf *messages);
char       *session_prepare_path(const session *s, const char *subdir, const char *stem, const char *ext);

#endif
