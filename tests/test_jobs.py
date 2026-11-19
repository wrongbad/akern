import json
import os
import signal

from conftest import seen, seen_pending, send_now, tool, parallel, wait_for


def test_a_call_not_waited_for_is_pending_and_its_result_wakes_the_agent_after_eof(kernel):
    run = kernel(tool("exec", argv=["sleeper", "0.3"], wait=False))
    [pending] = run.pending
    assert pending["pid"] > 0
    [(late, result)] = run.job_reports
    assert (late["tool_use_id"], late["status"]) == (pending["tool_use_id"], "exit 0")
    assert result["stdout"] == "slept 0.3 seconds\n"
    assert run.replies[-1].startswith("The tool said:")


def test_send_now_preempts_a_stream_keeping_the_partial_text(kernel, script):
    env = script(
        {"type": "text_delta", "text": "I'll rename the module and then "},
        {"x-sleep": 5},
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

    run = kernel(parallel(*(tool("exec", argv=["gate", str(gate)]) for gate in gates)),
                 later=[(seen("tool_progress", 2), "skip it")], during=release_gates)
    assert len(run.pending) == 2 and not run.tool_results
    assert "echo: skip it" in run.replies
    assert sorted(result["stdout"] for _, result in run.job_reports) == ["released gate-a\n", "released gate-b\n"]


def test_a_job_killed_by_its_process_group_still_reports(kernel):
    def kill_the_job(run):
        [pending] = wait_for(lambda: run.pending)
        os.killpg(pending["pid"], signal.SIGTERM)

    run = kernel(tool("exec", argv=["sh", "-c", "sleep 30 & wait"], wait=False), during=kill_the_job)
    [(_, result)] = run.job_reports
    assert result["signal"] == signal.SIGTERM


def test_stdin_then_writes_then_close_reach_the_program_in_order(kernel):
    run = kernel(tool("exec", argv=["sed", "s/^/got:/"], stdin="one\n", stdin_open=True, wait=False),
                 later=[(seen_pending(), lambda run: tool("write", job=run.pending[0]["tool_use_id"],
                                                          content="two\n", close=True))])
    assert run.pending[0]["stdin_open"] is True
    [(_, result)] = run.job_reports
    assert result["stdout"] == "got:one\ngot:two\n"


def test_a_line_typed_during_the_stream_waits_for_the_calls_and_goes_with_their_results(kernel, script, tmp_path):
    gate = tmp_path / "gate"
    call = {"type": "tool_use", "id": "hi", "name": "exec", "input": {"argv": ["echo", "hi"]}}
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
    assert json.loads(result["content"][0]["text"])["stdout"] == "hi\n"
    [delivery] = [m for m in run.messages if any(b.get("type") == "tool_result" for b in m["content"])]
    assert delivery["content"][1:] == [{"type": "text", "text": "also this"}]


def test_killing_a_job_still_waiting_for_a_slot_means_it_never_runs(kernel, tmp_path):
    gate = tmp_path / "gate"
    run = kernel(parallel(tool("exec", argv=["gate", str(gate)]), tool("exec", argv=["sleeper", "0"], wait=False)),
                 "--max-processes", "1",
                 later=[(seen("tool_start", 2), lambda run: json.dumps({"type": "kill", "job": run.calls[1], "ref": 1}))],
                 during=lambda run: wait_for(lambda: run.events_of("ok")) and gate.write_text("released\n"))
    assert run.events_of("ok") == [{"type": "ok", "ref": 1}]
    assert [e["call"] for e in run.events_of("tool_progress")] == [run.calls[0]]
    assert run.result_texts[1] == "Not run: killed before it started."
    assert [e["status"] for e in run.events_of("tool_end")] == ["not run", "done"]
