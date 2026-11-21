/* session: the session log (akern.md §5), which this kernel alone appends to while it
   runs. Owns the ownership lease, appending entries of any type, the head, reading the
   context and the settings entries on its chain, and whether an append has failed: from then on it
   appends nothing and an append returns NULL. What to append, and when, is decided by turns;
   stopping when the log fails is the loop's. head = the last entry in the file; context = the
   messages on the parent chain from the head back to a root, each as its newest replace entry has
   it. */

#ifndef SESSION_H
#define SESSION_H

#include <stdint.h>
#include <sys/types.h>

#include "main.h"

#define AT_HEAD NULL
#define AS_ROOT ""

enum { MAX_ID_CHARS = 32 };

typedef struct session {
    int fd, owner_lock_fd;
    bool failed, ends_mid_line;
    off_t file_size;
    struct entry *entries;
    size_t nentries, capentries;
    size_t *slots, nslots;
    uint64_t id_rng;
    char head_id[MAX_ID_CHARS + 1];
    const config *config;
} session;

/* Opens or creates the file, takes the owner lock, indexes every entry. */
void session_open(session *s);
/* Gives up ownership: empties the owner file. */
void session_close(session *s);
/* The head entry's id; "" for an empty session. */
const char *session_get_head_id(const session *s);
/* Appends one entry line; its id, or NULL once a write has failed. */
const char *session_append(
    session *s,
    const char *type,
    const char *parent_id,
    const json_list *members
);
/* True if an entry has this id. */
bool session_has_entry(const session *s, const char *id);
/* True once an append has failed. */
bool session_has_failed(const session *s);
/* Writes the current message of a message entry; false otherwise. */
bool session_read_message(const session *s, const char *id, json_buf *message);
/* Writes the chain's current messages, oldest first, and their ids. */
void session_read_context(const session *s, json_buf *messages, strv *ids);
/* Lists the chain's settings entries, oldest first. */
void session_read_settings(const session *s, json_list *entries);

#endif
