/* input: everything the kernel reads from stdin (akern.md §3): commands and prompt
   lines. Owns reading and splitting stdin, parsing commands and replying to
   them, except an append's ok, and stdin's end. Replies go out through output; what a
   command does to the conversation belongs to turns and inbox. */

#ifndef INPUT_H
#define INPUT_H

#include "loop.h"

typedef struct input {
    buf stdin_pending;
    bool stdin_eof, skip_rest_of_line;
    const config *config;
    struct output *output;
    struct inbox *inbox;
    struct turns *turns;
} input;

/* Watches stdin until EOF; each complete line runs at once, in order. */
void input_watch(input *c, poll_set *set);
/* True once stdin has ended. */
bool input_has_ended(const input *c);

#endif
