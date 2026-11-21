/* main: akern's entry point and the only code that reads argv and the environment. Owns
   the options and environment defaults, among them a new session file in $TMPDIR without -s,
   the AKERN_HOME and recursion limit children inherit, and building the kernel: every module's
   state in one
   root, each module wired to the siblings it uses. The config it fills is read-only once the
   kernel runs; what the set command changes, and the AKERN_MODEL children inherit, are the
   turns'. */

#ifndef MAIN_H
#define MAIN_H

#include "json.h"

/* The longest stdin or provider line, and the most exec output kept in memory: a guard
   against running out of memory, larger than any request's media. */
#define MAX_LINE_BYTES ((size_t)64 << 20)

typedef struct config {
    char *session_path;
    char *home_dir;
    char *provider_model;
    char *system_path;
    json_list param_members;
    int max_processes;
    double kill_grace_seconds;
    bool print_answers;
    int recursion_limit;
} config;

#endif
