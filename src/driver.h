/* driver: the conversation state machine (kernel.md, concurrency.md): runs and turns, with
   one model request in flight at a time. Owns when queued items are delivered, building each
   request and running the provider, appending its replies and the user messages that answer
   them, preemption, aborts, the run's outcome, and closing every pending call at shutdown.
   Also owns the provider settings the set command changes: model, system prompt path and
   params, which start from the config. Running tools is calls'; queueing is inbox's; storage
   is session's.
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
    json_list       returned_contents;
    buf             answer_text;
    bool            last_run_failed;
    size_t          inbox_items_before_wait;
    char           *provider_model, *system_path;
    json_list       param_members;
    const config   *config;
    struct sink    *events;
    struct ctl     *ctl;
    struct calls   *calls;
    struct inbox   *inbox;
    struct procs   *procs;
    struct session *session;
} driver;

bool driver_advance(driver *d);
void driver_abort(driver *d, enum abort_jobs jobs);
void driver_stop(driver *d);
void driver_append_leftover_results(driver *d);
void driver_status(const driver *d, json_buf *status);
bool driver_quiescent(const driver *d);
bool driver_ran(const driver *d);
int  driver_exit_code(const driver *d);

#endif
