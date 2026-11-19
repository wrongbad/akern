/* driver: the conversation state machine (kernel.md): runs and turns, with
   one model request in flight at a time. Owns when queued items are delivered, building each
   request and running the provider, appending its replies and the user messages that answer
   them, preemption, aborts, the run's outcome, and closing every pending call at shutdown.
   When the provider names a request block the API rejected, appends a replace entry that swaps
   it for a note and sends the request again, a few times at most.
   Owns every append to the session and the ok that answers an append command, holding one
   that names a parent until a turn boundary. Also owns the provider settings the set command
   changes: model, system prompt text and params, which start from the config, and the
   AKERN_MODEL children inherit. The system text is read once at open: the -S file, else the
   output of the prompt program found in AKERN_HOME, run to completion before the loop starts. Running tools is calls'; queueing is inbox's; storage is
   session's.
   run = from a delivery while idle to run_end; turn = one provider request and its reply;
   quiescent = idle, with nothing due and no job running. */

#ifndef DRIVER_H
#define DRIVER_H

#include "main.h"

enum abort_jobs { KEEP_JOBS, KILL_JOBS };

typedef enum { S_IDLE, S_STREAM, S_ABORTING, S_WAITING } dstate;

typedef struct driver {
    dstate          state;
    int             run, turn;
    bool            abort_requested, abort_kills_jobs, abort_ends_run;
    struct worker  *provider;
    bool            provider_exited;
    json_buf        provider_result, stripped_message, error;
    strv            context_ids;
    int             block_retries;
    json_list       returned_contents;
    buf             answer_text;
    bool            last_run_failed;
    struct held_entry *held;
    size_t          nheld, capheld;
    char           *provider_model;
    buf             system;
    json_list       param_members;
    const config   *config;
    struct sink    *sink;
    struct calls   *calls;
    struct inbox   *inbox;
    struct procs   *procs;
    struct session *session;
} driver;

void driver_open(driver *d);
bool driver_set(driver *d, json model, json system_path, json params);
bool driver_append(driver *d, const char *type, const char *parent_id, const json_list *members, json ref);
bool driver_advance(driver *d);
void driver_abort(driver *d, enum abort_jobs jobs);
void driver_stop(driver *d);
void driver_append_leftover_results(driver *d);
void driver_status(const driver *d, json_buf *status);
bool driver_quiescent(const driver *d);
bool driver_ran(const driver *d);
int  driver_exit_code(const driver *d);

#endif
