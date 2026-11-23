"""The bus: the session file is the stream. Every line the kernel writes to stdout goes to the
file as written, in the same order, partial lines included. A reply sits in the context at its
first line. Every run is checked for the first of these by invariants.log_violations; these tests
show the cases."""

import json

from conftest import PATIENCE, parallel, seen, send_now, shell, tool, wait_for


def test_the_file_is_the_stream(kernel):
    run = kernel(parallel(shell("echo one"), shell("echo two")), later=[(seen("run_end"), "hello")])
    assert run.entries == run.events


def test_an_uninterrupted_turn_keeps_its_partial_lines_in_the_file(kernel):
    run = kernel(shell("echo hi"))
    types = [e["type"] for e in run.entries]
    assert {"turn_start", "text_delta", "tool_start", "tool_progress", "block"} <= set(types)
    assert [e["message"]["role"] for e in run.entries_of("message")] == ["user", "assistant", "user", "assistant"]


def test_a_now_message_mid_stream_lands_after_the_partial_reply_in_the_context(kernel, home, script):
    env = script(
        {"type": "text_delta", "text": "I'll start "},
        {"x-sleep": PATIENCE},  # a window for the send; the preemption ends it
        {"type": "result", "stop": "end_turn", "message": {"role": "assistant", "content": []}},
        {"type": "text_delta", "text": "ok"},
        {"type": "result", "stop": "end_turn", "message": {"role": "assistant", "content": [{"type": "text", "text": "ok"}]}},
    )
    run = kernel("go", later=[(seen("text_delta"), send_now("stop"))], env={**home.env, **env})
    types = [e["type"] for e in run.entries]
    assert types.index("turn_start") < types.index("preempt")
    [partial] = [e for e in run.entries_of("message") if e.get("meta", {}).get("stop") == "aborted"]
    [now] = [e for e in run.entries_of("message") if e["message"]["content"][0].get("text") == "stop"]
    assert run.entries.index(now) < run.entries.index(partial)  # sealed later, placed earlier
    second = home.requests[1]["messages"]
    assert [(m["role"], m["content"][0]["text"]) for m in second] == [
        ("user", "go"), ("assistant", "I'll start "), ("user", "stop")]


def test_a_now_message_while_calls_run_makes_them_jobs_named_in_a_pending_line(kernel, tmp_path):
    gate = tmp_path / "gate"

    def release(run):
        wait_for(lambda: run.events_of("pending"))
        gate.write_text("released\n")

    run = kernel(tool("shell", command=f"gate {gate}"),
                 later=[(seen("tool_progress"), send_now("skip it"))], during=release)
    [pending] = run.events_of("pending")
    assert pending["calls"] == run.calls
    [now] = [e for e in run.entries_of("message") if e["meta"].get("items") and e["message"]["content"][0].get("text") == "skip it"]
    assert now["meta"]["source"] == "inbox"  # no results: the call was still running
    [(late, result)] = run.job_reports
    assert (late["tool_use_id"], result["output"].splitlines()[-1]) == (run.calls[0], "released")
    assert "echo: skip it" in run.replies


def test_resuming_replays_the_file_into_the_context_with_this_starts_params(kernel, home, script):
    env = script(
        {"type": "text_delta", "text": "half "},
        {"x-sleep": PATIENCE},  # a window for the send; the preemption ends it
        {"type": "result", "stop": "end_turn", "message": {"role": "assistant", "content": []}},
        {"type": "result", "stop": "end_turn", "message": {"role": "assistant", "content": [{"type": "text", "text": "done"}]}},
    )
    first = kernel("go", "-P", "effort=high", later=[(seen("text_delta"), send_now("stop"))], env={**home.env, **env})
    kernel("again", "-P", "effort=low", session=first.session, env=home.env)
    assert home.requests[0]["params"] == {"effort": "high"}
    last = home.requests[-1]
    assert [(m["role"], m["content"][0]["text"]) for m in last["messages"]] == [
        ("user", "go"), ("assistant", "half "), ("user", "stop"), ("assistant", "done"), ("user", "again")]
    assert last["params"] == {"effort": "low"}
    lines = first.session.read_text().splitlines()
    assert all(json.loads(line) for line in lines)
