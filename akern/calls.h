/* calls: every tool call, from its tool_use block to its answer (tools.md §1). Owns running
   each shell and edit call as its tool's program, starting it at once or refusing it past
   --max-processes, keeping its output and spilling it to files in <session>.d/calls; the job
   tool (input, end of input, a signal, a wait); killing jobs, answering each call of the turn,
   and queueing a job's job_result in the inbox when it ends. Processes are proc's; when the
   turn is answered is turns'.
   job = a call answered with tool_pending, or started with wait: false, named by its call id;
   the turn is settled when every waited call has finished. */

#ifndef CALLS_H
#define CALLS_H

#include "main.h"

typedef struct calls {
    struct call **live, **turn;
    size_t nlive, caplive, nturn, capturn;
    int running_execs;
    const struct tools *tools;
    const config *config;
    struct output *output;
    struct procs *procs;
    struct inbox *inbox;
} calls;

/* The tools array sent with every request. */
json calls_get_tool_schemas(calls *cs);
/* Starts one call per tool_use block, in order; returns the turn's count. */
size_t calls_start(calls *cs, json assistant_message, int run, int turn);
/* The earliest time a job or shell call's wait runs out, or 0. */
double calls_get_next_deadline(const calls *cs);
/* Collects calls whose program exited, ends job calls whose wait ran out, and stops waiting
   for shell calls whose wait ran out (they become jobs); true if any of that happened. */
bool calls_advance(calls *cs);
/* True when every waited call of the turn is done or cancelled. */
bool calls_is_turn_settled(const calls *cs);
/* Kills the turn's unfinished waited calls; wait: false ones keep running. */
void calls_abort_turn(calls *cs);
/* Adds each turn call's tool_result or tool_pending; empties the turn. */
void calls_take_turn_results(calls *cs, json_list *blocks);
/* Terminates every job that has a process. */
void calls_kill_jobs(calls *cs);
/* Counts jobs not yet done or cancelled. */
size_t calls_count_running_jobs(const calls *cs);
/* Writes the status event's calls array: this turn's calls. */
void calls_put_turn_json(const calls *cs, json_buf *turn);
/* Writes the status event's jobs array, with their pids. */
void calls_put_jobs_json(const calls *cs, json_buf *jobs);
/* At exit, with every worker gone: gives each pending call its result. */
void calls_close(calls *cs);

#endif
