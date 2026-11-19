/* ctl: the kernel's control channel in (protocols.md §3, akern-loop.md §3): commands and
   prompt lines on stdin. Owns reading and splitting stdin, parsing commands and replying to
   them, except an append's ok, and stdin's end. Replies go out through the sink; what a
   command does to the conversation belongs to the driver, calls and inbox. */

#ifndef CTL_H
#define CTL_H

#include "loop.h"

typedef struct ctl {
    buf            stdin_pending;
    bool           stdin_eof, skip_rest_of_line;
    const config  *config;
    struct sink   *sink;
    struct inbox  *inbox;
    struct driver *driver;
    struct calls  *calls;
} ctl;

void ctl_watch(ctl *c, poll_set *set);
bool ctl_eof(const ctl *c);

#endif
