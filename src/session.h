/* session: the session log (protocols.md §2), which this kernel alone appends to while it
   runs. Owns the ownership lease, appending entries of any type, the head, building the
   context, and whether an append has failed: from then on it appends nothing and an append
   returns NULL. What to append, and when, is decided by the driver; stopping when the log
   fails is the loop's.
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
    int            fd, owner_lock_fd;
    bool           failed;
    off_t          file_size;
    struct entry  *entries;
    size_t         nentries, capentries;
    size_t        *slots, nslots;
    uint64_t       id_rng;
    char           head_id[MAX_ID_CHARS + 1];
    const config  *config;
} session;

void        session_open(session *s);
void        session_close(session *s);
const char *session_head_id(const session *s);
const char *session_append(session *s, const char *type, const char *parent_id, const json_list *members);
bool        session_has_entry(const session *s, const char *id);
bool        session_failed(const session *s);
void        session_context(const session *s, json_buf *messages);

#endif
