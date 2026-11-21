import json
import signal

import pytest

from conftest import PATIENCE, parallel, seen, shell, tool, wait_for


def command(type, **fields):
    return json.dumps({"type": type, **fields})


def command_error(message, **ref):
    return {"type": "error", "where": "command", "message": message, **ref}


@pytest.mark.parametrize("line, error", [
    ('{"type": "send"', command_error("invalid JSON")),
    (json.dumps({"ref": 1}), command_error("unknown command", ref=1)),
    (command("launch", ref="r"), command_error("unknown command", ref="r")),
    (command("send", content=5, ref=2), command_error("content must be a string or an array of content blocks", ref=2)),
    (command("set", model="mock/other", ref=3), command_error("unknown command", ref=3)),
    (command("append", entry={"text": "untyped"}, ref=4),
     command_error("entry needs a type, and parent must be null or an existing id", ref=4)),
    (command("append", entry={"type": "x-note", "parent": "nope"}, ref=5),
     command_error("entry needs a type, and parent must be null or an existing id", ref=5)),
], ids=["invalid-json", "no-type", "unknown-type", "bad-content", "removed-command", "untyped-entry", "unknown-parent"])
def test_a_failed_command_is_one_error_and_changes_nothing(kernel, line, error):
    run = kernel(line)
    assert run.events_of("error") == [error]
    assert not run.events_of("queued")
    assert not run.entries_of("x-note")


@pytest.mark.parametrize("field, value, message", [
    ("when", "later", "when must be now, turn, idle or deferred"),
    ("source", "robot", "source must be prompt or inject"),
])
def test_send_rejects_an_unrecognized_when_or_source(kernel, field, value, message):
    run = kernel(command("send", content="hi", ref=7, **{field: value}))
    assert run.events_of("error") == [command_error(message, ref=7)]
    assert not run.events_of("run_start")


def test_a_line_longer_than_64_mib_is_dropped_whole_with_one_error(kernel):
    run = kernel("x" * ((64 << 20) + 1), later=[(0, "hello")])
    assert run.events_of("error") == [command_error("line longer than 64 MiB")]
    assert run.replies == ["echo: hello"]


def test_an_item_is_queued_on_arrival_right_after_its_ok(kernel):
    run = kernel(command("send", content="hello", source="inject", when="idle", ref=1))
    ok, queued = [e for e in run.events if e["type"] in ("ok", "queued")]
    assert ok == {"type": "ok", "ref": 1}
    assert queued == {"type": "queued", "source": "inject", "when": "idle", "n": 1}
    assert run.replies == ["echo: hello"]


def test_abort_kills_running_calls_and_hands_back_queued_items(kernel):
    run = kernel(shell("sleep 30"), later=[(seen("tool_start"), command("send", content="and then this", when="idle")),
                                           (seen("queued", 2), command("abort"))])
    [end] = run.events_of("run_end")
    assert (end["reason"], end["returned"]) == ("aborted", ["and then this"])
    assert run.result_text == "Killed: the run was aborted."
    assert [e["status"] for e in run.events_of("tool_end")] == ["killed"]


def test_abort_hands_back_only_user_items_and_keeps_a_finished_jobs_result_for_later(kernel, script):
    job = {"type": "tool_use", "id": "job", "name": "shell", "input": {"command": "echo done", "wait": 0}}
    env = script(
        {"type": "result", "stop": "tool_use", "message": {"role": "assistant", "content": [job]}},
        {"type": "text_delta", "text": "thinking"},
        {"x-sleep": PATIENCE},  # a window for the prompt and abort; the abort ends it
        {"type": "result", "stop": "end_turn", "message": {"role": "assistant", "content": []}},
    )
    run = kernel("start", env=env, later=[(seen("text_delta"), command("send", content="and then this", when="idle")),
                                          (seen("queued", 3), command("abort"))])
    [end] = run.events_of("run_end")
    assert (end["reason"], end["returned"]) == ("aborted", ["and then this"])
    assert len(run.events_of("run_start")) == 1
    [(late, result)] = run.job_reports
    assert (late["tool_use_id"], result["output"]) == ("job", "done\n")


@pytest.mark.parametrize("jobs, outcome", [
    (False, {"exit": 0}),
    (True, {"signal": signal.SIGTERM, "killed": True}),
])
def test_abort_keeps_jobs_unless_jobs_is_true(kernel, tmp_path, jobs, outcome):
    gate = tmp_path / "gate"
    run = kernel(parallel(tool("shell", command=f"gate {gate}", wait=0), shell("sleeper 30")),
                 later=[(seen("tool_progress", 2), command("abort", jobs=jobs))],
                 during=lambda run: wait_for(lambda: run.events_of("run_end")) and gate.write_text("released\n"))
    [(_, result)] = run.job_reports
    assert result.items() >= outcome.items()
    assert jobs or result["output"].endswith("released\n")
    assert [e["reason"] for e in run.events_of("run_end")] == ["aborted", "done"]


def test_status_reports_the_driver_state_with_this_turns_calls_and_live_jobs(kernel):
    run = kernel(parallel(tool("shell", command="sleeper 30", wait=0), shell("sleeper 30")),
                 later=[(seen("tool_progress", 2), command("status")),
                        (seen("status"), command("abort", jobs=True))])
    job_call, call = run.calls
    assistant = [e for e in run.entries_of("message") if e["message"]["role"] == "assistant"][0]
    [status] = run.events_of("status")
    [job] = status.pop("jobs")
    assert status == {"type": "status", "state": "waiting", "run": 1, "turn": 1, "model": "mock/echo",
                      "head": assistant["id"], "inbox": 0, "calls": [
                          {"call": job_call, "name": "shell", "state": "running"},
                          {"call": call, "name": "shell", "state": "running"}]}
    assert job["pid"] > 0
    assert (job["call"], job["state"]) == (job_call, "running")


def settings(**fields):
    return command("append", entry={"type": "settings", **fields})


def test_a_settings_entry_changes_the_model_and_params_from_the_next_request(kernel, home):
    run = kernel("first", "-P", "effort=high", env=home.env,
                 later=[(seen("run_end"), settings(model="mock/other", params={"max_tokens": 5})), (0, "second")])
    assert [(r["model"], r["params"]) for r in home.requests] == [("echo", {"effort": "high"}), ("other", {"max_tokens": 5})]
    assert run.entries_of("message")[-1]["meta"]["model"] == "mock/other"


def test_a_settings_entry_changes_only_the_fields_it_names(kernel, home, tmp_path):
    (tmp_path / "old.md").write_text("old")
    kernel("first", "-S", "old.md", "-P", "effort=high", env=home.env,
           later=[(seen("run_end"), settings(system="new")), (0, "second")])
    assert [(r["model"], r["system"], r["params"]) for r in home.requests] == [
        ("echo", "old", {"effort": "high"}), ("echo", "new", {"effort": "high"})]


def test_settings_follow_the_branch_they_were_appended_on(kernel, home):
    kernel("one", env=home.env,
           later=[(seen("run_end"), settings(model="mock/other")), (0, "two"),
                  (seen("run_end", 2), lambda run: command("append", entry={"type": "branch", "parent": run.entries_of("message")[1]["id"]})),
                  (0, "three")])
    assert [r["model"] for r in home.requests] == ["echo", "other", "echo"]


def test_a_resumed_session_keeps_its_settings_and_flags_record_only_what_differs(kernel, home):
    first = kernel("one", "-P", "effort=high", env=home.env)
    kernel("two", env=home.env, session=first.session, model=None)
    kernel("three", "-m", "mock/echo", env=home.env, session=first.session)
    kernel("four", "-m", "mock/other", env=home.env, session=first.session)
    assert [(r["model"], r["params"]) for r in home.requests] == [
        ("echo", {"effort": "high"}), ("echo", {"effort": "high"}), ("echo", {"effort": "high"}), ("other", {"effort": "high"})]
    picked = [{k: s[k] for k in ("model", "params") if k in s} for s in first.entries_of("settings")]
    assert picked == [{"model": "mock/echo", "params": {"effort": "high"}}, {"model": "mock/other"}]


def test_a_fresh_session_without_a_model_exits_2(kernel):
    run = kernel("hi", model=None, code=2)
    assert "no model" in run.stderr


def test_append_writes_at_the_head_or_as_a_new_root_and_replies_only_with_a_ref(kernel):
    run = kernel(command("append", entry={"type": "x-note", "text": "a"}),
                 later=[(0, command("append", entry={"type": "x-note", "text": "b", "id": "forged"}, ref="b")),
                        (0, command("append", entry={"type": "x-note", "text": "c", "parent": None}, ref="c"))])
    a, b, c = run.entries_of("x-note")
    assert run.events_of("ok") == [{"type": "ok", "id": b["id"], "ref": "b"}, {"type": "ok", "id": c["id"], "ref": "c"}]
    assert (b["parent"], c["parent"]) == (a["id"], None)
    assert [n["text"] for n in (a, b, c)] == ["a", "b", "c"]
