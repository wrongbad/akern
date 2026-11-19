/* main: akern-loop's entry point and the only code that reads argv and the environment. Owns
   the options and environment defaults, the recursion limit, the environment children inherit,
   and building the kernel: every module's state in one root, each module wired to the siblings
   it uses. The config it fills is read-only once the kernel runs; what the set command changes
   is the driver's. */

#ifndef MAIN_H
#define MAIN_H

#include "json.h"

typedef struct config {
    const char *session_path;
    const char *tmp_dir;
    char       *home_dir;
    char       *provider_model;
    char       *system_path;
    json_list   param_members;
    int         max_processes;
    double      kill_grace_seconds;
    size_t      max_line_bytes;
    bool        print_answers;
    int         recursion_limit;
} config;

#endif
