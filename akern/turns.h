/* turns: the state machine of runs and turns (akern.md), with one model request in
   flight at a time. Owns when queued items are delivered, building each request and running
   the provider, appending its replies and the user messages that answer them, preemption,
   aborts, the run's outcome, and closing every pending call at shutdown.
   When the provider names a request block the API rejected, appends a replace entry that swaps
   it for a note and sends the request again, a few times at most.
   Owns every append to the session and the ok that answers an append command, holding one
   that names a parent until a turn boundary. Also owns the provider settings that a settings
   entry changes when it is appended: model, system prompt text and params, which start from
   the config, and the AKERN_MODEL children inherit. The system text is read once at open: the
   -S file, else the output of the system-prompt program found in AKERN_HOME, run to completion before
   the loop starts. Running tools is calls'; queueing is inbox's; storage is session's.
   run = from a delivery while idle to run_end; turn = one provider request and its reply;
   quiescent = idle, with nothing due and no job running. */

#ifndef TURNS_H
#define TURNS_H

#include "main.h"

enum abort_jobs { KEEP_JOBS, KILL_JOBS };

typedef enum { S_IDLE, S_STREAM, S_ABORTING, S_WAITING } dstate;

typedef struct turns {
    dstate state;
    int run, turn;
    bool abort_requested, abort_kills_jobs, abort_ends_run;
    struct worker *provider;
    bool provider_exited;
    json_buf provider_result, stripped_message, error;
    strv context_ids;
    int block_retries;
    json_list returned_contents;
    buf answer_text;
    bool last_run_failed;
    struct held_entry *held;
    size_t nheld, capheld;
    char *provider_model;
    buf system, startup_system;
    json_list param_members;
    const config *config;
    struct output *output;
    struct calls *calls;
    struct inbox *inbox;
    struct procs *procs;
    struct session *session;
} turns;

/* Before the session opens: reads the startup system text, or dies. */
void turns_open(turns *d);
/* Appends one settings entry with the startup settings that differ. */
void turns_apply_startup_settings(turns *d);
/* Appends an entry, or holds one with a parent until a turn boundary. */
bool turns_append(
    turns *d,
    const char *type,
    const char *parent_id,
    const json_list *members,
    json ref
);
/* One step of the state machine; true if anything moved. */
bool turns_advance(turns *d);
/* Requests an abort at the next step; KILL_JOBS also kills every job. */
void turns_abort(turns *d, enum abort_jobs jobs);
/* At shutdown: closes an open run as aborted. */
void turns_stop(turns *d);
/* At exit: closes every pending call with its job result. */
void turns_append_leftover_results(turns *d);
/* Writes the status event object. */
void turns_put_status(const turns *d, json_buf *status);
/* True when idle, with nothing due and no job running. */
bool turns_is_quiescent(const turns *d);
/* The latest run's number; 0 before the first. */
int turns_get_run(const turns *d);
/* True once a run has started. */
bool turns_has_run(const turns *d);
/* 1 if the last run ended in error, else 0. */
int turns_get_exit_code(const turns *d);

#endif
