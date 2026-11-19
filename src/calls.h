/* calls: the model's tools, exec, read and write (tools.md §1), and every tool call from
   its tool_use block to its answer. Owns the tool definitions sent to the provider, running
   exec programs within --max-processes, keeping their output and spilling it to files in
   <session>.d/calls, writing to a job's stdin, killing jobs, answering each call of the turn,
   and queueing a job's job_result in the inbox when it ends. Reading and writing paths is files'; processes are proc's; when the turn
   is answered is the driver's.
   job = a call answered with tool_pending, or started with wait: false, named by its call id;
   the turn is settled when every waited call has finished and every other one has started. */

#ifndef CALLS_H
#define CALLS_H

#include "main.h"

typedef struct calls {
    struct call   **live, **turn;
    size_t          nlive, caplive, nturn, capturn;
    int             running_execs;
    json_buf        tool_schemas;
    const config   *config;
    struct sink    *sink;
    struct procs   *procs;
    struct inbox   *inbox;
} calls;

json   calls_tool_schemas(calls *cs);
size_t calls_start(calls *cs, json assistant_message, int run, int turn);
bool   calls_advance(calls *cs);
bool   calls_turn_settled(const calls *cs);
void   calls_abort_turn(calls *cs);
void   calls_take_turn_results(calls *cs, json_list *blocks);
bool   calls_kill_job(calls *cs, json call_id);
void   calls_kill_jobs(calls *cs);
size_t calls_running_jobs(const calls *cs);
void   calls_turn_json(const calls *cs, json_buf *turn);
void   calls_jobs_json(const calls *cs, json_buf *jobs);
void   calls_close(calls *cs);

#endif
