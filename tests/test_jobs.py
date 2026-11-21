import json
import os
import signal

import pytest

from conftest import PATIENCE, seen, seen_pending, send_now, tool, parallel, wait_for


def test_a_call_not_waited_for_is_pending_and_its_result_wakes_the_agent_after_eof(kernel):
    run = kernel(tool("shell", command="sleeper 0.3", wait=0))
    [pending] = run.pending
    assert pending["pid"] > 0
    [(late, result)] = run.job_reports
    assert (late["tool_use_id"], late["status"]) == (pending["tool_use_id"], "exit 0")
    assert result["output"] == "sleeping 0.3\nslept 0.3 seconds\n"
    assert run.replies[-1].startswith("The tool said:")


def test_send_now_preempts_a_stream_keeping_the_partial_text(kernel, script):
    env = script(
        {"type": "text_delta", "text": "I'll rename the module and then "},
        {"x-sleep": PATIENCE},  # a window for the send; the preemption ends it
        {"type": "text_delta", "text": "never reached"},
        {"type": "result", "stop": "end_turn",
         "message": {"role": "assistant", "content": [{"type": "text", "text": "never reached"}]}},
        {"type": "text_delta", "text": "ok, snake_case"},
        {"type": "result", "stop": "end_turn",
         "message": {"role": "assistant", "content": [{"type": "text", "text": "ok, snake_case"}]}},
    )
    run = kernel("rename the module", later=[(seen("text_delta"), send_now("use snake_case"))], env=env)
    assert run.events_of("preempt")
    [aborted] = [e for e in run.entries_of("message") if e.get("meta", {}).get("stop") == "aborted"]
    assert aborted["message"]["content"][0]["text"] == "I'll rename the module and then "
    assert run.replies[-1] == "ok, snake_case"
    assert "never reached" not in run.streamed


def test_a_line_typed_while_calls_run_is_answered_at_once_and_their_results_arrive_later(kernel, tmp_path):
    gates = [tmp_path / "gate-a", tmp_path / "gate-b"]

    def release_gates(run):
        wait_for(lambda: seen_pending(2)(run))
        for gate in gates:
            gate.write_text(f"released {gate.name}\n")

    run = kernel(parallel(*(tool("shell", command=f"gate {gate}") for gate in gates)),
                 later=[(seen("tool_progress", 2), "skip it")], during=release_gates)
    assert len(run.pending) == 2 and not run.tool_results
    assert "echo: skip it" in run.replies
    assert sorted(result["output"].splitlines()[-1] for _, result in run.job_reports) == ["released gate-a", "released gate-b"]


def test_a_job_killed_by_its_process_group_still_reports(kernel):
    def kill_the_job(run):
        [pending] = wait_for(lambda: run.pending)
        wait_for(lambda: run.events_of("tool_progress"))  # running: not still inside exec
        os.killpg(pending["pid"], signal.SIGTERM)

    run = kernel(tool("shell", command="echo ready >&2 && sleep 30 & wait", wait=0), during=kill_the_job)
    [(_, result)] = run.job_reports
    assert result["signal"] == signal.SIGTERM


def test_stdin_then_job_stdin_then_eof_reach_the_program_in_order(kernel):
    run = kernel(tool("shell", command="sed s/^/got:/", stdin="one\n", eof=False, wait=0),
                 later=[(seen_pending(), lambda run: tool("job", id=run.pending[0]["tool_use_id"],
                                                          stdin="two\n", eof=True))])
    job_id = run.pending[0]["tool_use_id"]
    assert run.pending[0]["stdin_open"] is True
    assert run.result_texts == [f"{job_id}: wrote 4 bytes to its stdin, closed its stdin."]
    [(_, result)] = run.job_reports
    assert result["output"] == "got:one\ngot:two\n"


def test_a_shell_call_whose_wait_runs_out_becomes_a_job(kernel):
    run = kernel(tool("shell", command="sleep 0.5 && echo done", wait=0.1))
    assert len(run.pending) == 1 and not run.tool_results
    [(_, result)] = run.job_reports
    assert result["output"] == "done\n"


def test_job_signals_the_jobs_process_group_and_its_result_still_arrives(kernel):
    run = kernel(tool("shell", command="sleep 30 & wait", wait=0),
                 later=[(seen_pending(), lambda run: tool("job", id=run.pending[0]["tool_use_id"], signal="TERM"))])
    assert run.result_texts == [f"{run.pending[0]['tool_use_id']}: sent it SIGTERM."]
    [(_, result)] = run.job_reports
    assert result["signal"] == signal.SIGTERM


def test_job_wait_runs_out_or_ends_with_the_job_whose_result_comes_in_the_same_message(kernel):
    def job_call(**input):
        return lambda run: tool("job", id=run.pending[0]["tool_use_id"], **input)

    run = kernel(tool("shell", command="sleep 30", wait=0),
                 later=[(seen_pending(), job_call(wait=0.2)), (seen("tool_end"), job_call(signal="TERM", wait=5))])
    job_id = run.pending[0]["tool_use_id"]
    assert run.result_texts == [f"{job_id} still running after 0.2 s.",
                                f"{job_id}: sent it SIGTERM; ended: signal {signal.SIGTERM}."]
    [(_, result)] = run.job_reports
    assert result["signal"] == signal.SIGTERM
    [together] = [m for m in run.messages if any(b.get("type") == "job_result" for b in m["content"])]
    assert [b["type"] for b in together["content"]] == ["tool_result", "job_result"]


@pytest.mark.parametrize("input, reply", [
    ({"id": "nope"}, '"nope" is not a running job.'),
    ({"signal": "BOGUS"}, "Unknown signal: BOGUS"),
    ({"stdin": "x"}, "The stdin of {id} is closed."),
    ({}, "{id} is running."),
])
def test_job_reports_on_a_job_or_refuses(kernel, input, reply):
    run = kernel(tool("shell", command="sleep 30", wait=0),
                 later=[(seen_pending(), lambda run: tool("job", **{"id": run.pending[0]["tool_use_id"], **input})),
                        (seen("tool_end"), lambda run: tool("job", id=run.pending[0]["tool_use_id"], signal="KILL"))])
    assert run.result_texts[0] == reply.format(id=run.pending[0]["tool_use_id"])


def test_a_line_typed_during_the_stream_waits_for_the_calls_and_goes_with_their_results(kernel, script, tmp_path):
    gate = tmp_path / "gate"
    call = {"type": "tool_use", "id": "hi", "name": "shell", "input": {"command": "echo hi"}}
    env = script(
        {"type": "text_delta", "text": "running it"},
        {"x-wait": str(gate)},
        {"type": "result", "stop": "tool_use", "message": {"role": "assistant", "content": [call]}},
        {"type": "result", "stop": "end_turn", "message": {"role": "assistant", "content": [{"type": "text", "text": "ok"}]}},
    )
    run = kernel("start", env=env, later=[(seen("text_delta"), "also this")],
                 during=lambda run: wait_for(lambda: seen("queued", 2)(run)) and gate.write_text(""))
    assert not run.pending
    [result] = run.tool_results
    assert result["content"][0]["text"] == "[exit 0]\nhi\n"
    [delivery] = [m for m in run.messages if any(b.get("type") == "tool_result" for b in m["content"])]
    assert delivery["content"][1:] == [{"type": "text", "text": "also this"}]


def test_an_exec_past_max_processes_fails_at_once_and_never_runs(kernel, tmp_path):
    gate = tmp_path / "gate"
    run = kernel(parallel(tool("shell", command=f"gate {gate}"), tool("shell", command="sleeper 0")),
                 "--max-processes", "1",
                 during=lambda run: wait_for(lambda: run.events_of("tool_end")) and gate.write_text("released\n"))
    assert run.result_texts[1] == "Not run: already at the --max-processes limit (1)."
    assert [e["status"] for e in run.events_of("tool_end")] == ["not run", "done"]
    assert {e["call"] for e in run.events_of("tool_progress")} == {run.calls[0]}