import json
import shutil
import subprocess

from conftest import executable, seen, shell

PNG = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNgYGD4DwABBAEAwS2OUAAAAABJRU5ErkJggg=="
IMAGE = {"type": "image", "source": {"type": "base64", "media_type": "image/png", "data": PNG}}

STRICT = '''#!/usr/bin/env python3
"""Rejects the first image or document block of a request, as an API that names the block would."""
import json, os, sys
request = json.loads(sys.stdin.readline())
with open(os.environ["STRICT_LOG"], "a") as log:
    log.write(json.dumps(request["messages"]) + "\\n")
def first_media(messages):
    for i, message in enumerate(messages):
        for j, block in enumerate(message["content"] if isinstance(message["content"], list) else []):
            if block["type"] in ("image", "document"):
                return {"message": i, "block": j}
            for k, inner in enumerate(block.get("content") or []):
                if isinstance(inner, dict) and inner.get("type") in ("image", "document"):
                    return {"message": i, "block": j, "inner": k}
at = {"message": 0, "block": 1} if os.environ.get("STRICT_SAME") else first_media(request["messages"])
if at:
    print(json.dumps({"type": "error", "kind": "invalid_block", "message": "too big", "status": 400, "at": at}))
    sys.exit(1)
print(json.dumps({"type": "result", "stop": "end_turn", "message": {"role": "assistant", "content": [{"type": "text", "text": "ok"}]}}))
'''


def command(type, **fields):
    return json.dumps({"type": type, **fields})


def replace(target, text):
    return command("append", entry={"type": "replace", "target": target,
                                    "message": {"role": "user", "content": [{"type": "text", "text": text}]}})


def first_id(run):
    return run.events_of("message")[0]["id"]


def message(role, text, **meta):
    return command("append", entry={"type": "message", "message": {"role": role, "content": [{"type": "text", "text": text}]},
                                    **({"meta": meta} if meta else {})})


def texts(messages):
    return [m["content"][0]["text"] for m in messages]


def test_an_appended_entry_of_type_session_is_an_ordinary_entry(kernel, home):
    run = kernel(command("append", entry={"type": "session"}, ref=1), later=[(0, "hello")], env=home.env)
    header, appended = run.entries_of("session")
    assert run.events_of("ok") == [{"type": "ok", "id": appended["id"], "ref": 1}]
    assert [e["type"] for e in run.entries if "ts" in e][-3:] == ["session", "message", "message"]
    assert texts(home.requests[0]["messages"]) == ["hello"]


def test_the_newest_replace_entry_is_what_later_requests_send_and_after_a_reload(kernel, home):
    run = kernel("secret", later=[
        (seen("run_end"), lambda run: replace(first_id(run), "first")),
        (0, lambda run: replace(first_id(run), "redacted")),
        (0, lambda run: replace("nope", "ignored")),
        (0, "again")], env=home.env)
    redacted = {"role": "user", "content": [{"type": "text", "text": "redacted"}]}
    assert [r["messages"][0] for r in home.requests] == [
        {"role": "user", "content": [{"type": "text", "text": "secret"}]}, redacted]
    assert run.entries_of("message")[0]["message"]["content"][0]["text"] == "secret"
    shutil.copy(run.session, kernel.dir / f"run{kernel.runs + 1}.jsonl")
    kernel("resumed", env=home.env)
    assert home.requests[-1]["messages"][0] == redacted


def test_the_context_is_the_messages_after_the_last_reset_also_after_a_reload(kernel, home):
    run = kernel("one", later=[
        (seen("run_end"), command("append", entry={"type": "reset"}, ref="r")),
        (0, message("user", "summary")),
        (0, lambda run: message("assistant", run.replies[0], copy_of=run.entries_of("message")[1]["id"])),
        (0, "two"),
        (seen("run_end", 2), command("append", entry={"type": "reset"})),
        (0, "three")], env=home.env)
    [reset, _] = run.entries_of("reset")
    assert run.events_of("ok") == [{"type": "ok", "id": reset["id"], "ref": "r"}]
    one, two, three = home.requests
    assert texts(one["messages"]) == ["one"]
    assert texts(two["messages"]) == ["summary", run.replies[0], "two"]
    assert texts(three["messages"]) == ["three"]
    kernel("four", env=home.env, session=run.session)
    assert texts(home.requests[-1]["messages"]) == ["three", run.replies[-2], "four"]


def test_a_reset_appended_during_a_run_waits_for_a_turn_without_calls_and_later_appends_wait_behind_it(
    kernel, home, tmp_path
):
    gate = tmp_path / "gate"

    def open_gate_then(line):
        gate.write_text("open\n")
        return line

    run = kernel(shell(f"gate {gate}"), later=[
        (seen("tool_start"), command("append", entry={"type": "reset"}, ref="r")),
        (0, lambda run: open_gate_then(command("append", entry={"type": "x-note"}, ref="n"))),
        (seen("run_end"), "after")], env=home.env)
    kinds = [e["type"] if e["type"] != "message" else e["message"]["role"]
             for e in run.entries if e["type"] in ("message", "reset", "x-note")]
    assert kinds == ["user", "assistant", "user", "assistant", "reset", "x-note", "user", "assistant"]
    [reset], [note] = run.entries_of("reset"), run.entries_of("x-note")
    assert run.events_of("ok") == [{"type": "ok", "id": reset["id"], "ref": "r"}, {"type": "ok", "id": note["id"], "ref": "n"}]
    first_run = [e["type"] for e in run.events if e["type"] in ("ok", "run_end")]
    assert first_run[:3] == ["ok", "ok", "run_end"]
    assert texts(home.requests[-1]["messages"]) == ["after"]


def test_a_fork_is_the_first_lines_of_a_session_and_resumes_from_its_last_line(kernel, home):
    first = kernel("secret", later=[
        (seen("run_end"), lambda run: replace(first_id(run), "redacted")),
        (0, "two")], env=home.env)
    lines = first.session.read_text().splitlines()
    n = 1 + next(i for i, line in enumerate(lines) if json.loads(line)["type"] == "replace")
    fork = kernel.dir / "fork.jsonl"
    with open(fork, "w") as out:
        subprocess.run(["head", "-n", str(n), str(first.session)], stdout=out, check=True)
    kernel("three", env=home.env, session=fork)
    assert texts(home.requests[-1]["messages"]) == ["redacted", first.replies[0], "three"]
    assert first.session.read_text().splitlines() == lines


def strict(home, tmp_path, **env):
    executable(home.dir / "providers" / "strict", STRICT)
    return {**home.env, "STRICT_LOG": str(tmp_path / "strict.jsonl"), **env}


def requests(tmp_path):
    return [json.loads(line) for line in (tmp_path / "strict.jsonl").read_text().splitlines()]


def test_a_block_the_api_rejects_is_replaced_in_the_session_and_the_request_sent_again(kernel, home, tmp_path):
    result = {"type": "tool_result", "tool_use_id": "x", "is_error": False, "content": [{"type": "text", "text": "r"}, IMAGE]}
    run = kernel(command("send", content=[{"type": "text", "text": "look"}, IMAGE, result]), "-m", "strict/x",
                 env=strict(home, tmp_path))
    note = {"type": "text", "text": "[image omitted: too big]"}
    first, second = run.entries_of("replace")
    assert (first["target"], second["target"]) == (first_id(run), first_id(run))
    assert second["message"]["content"][1:] == [note, dict(result, content=[result["content"][0], note])]
    assert [m["content"][1:] for [m] in requests(tmp_path)][-1] == second["message"]["content"][1:]
    assert [e["type"] for e in run.events if e["type"] in ("turn_start", "error", "replace", "run_end")] == [
        "turn_start", "error", "replace", "turn_start", "error", "replace", "turn_start", "run_end"]
    assert run.events_of("run_end")[0]["reason"] == "done"
    assert run.replies == ["ok"]


def test_the_retries_stop_at_eight_replaced_blocks(kernel, home, tmp_path):
    run = kernel(command("send", content=[IMAGE] * 10), "-m", "strict/x", env=strict(home, tmp_path), code=1)
    assert len(run.entries_of("replace")) == 8
    assert len(requests(tmp_path)) == 9
    [end] = run.events_of("run_end")
    assert (end["reason"], end["error"]["kind"]) == ("error", "invalid_block")


def test_a_block_that_is_no_longer_media_is_not_replaced_again(kernel, home, tmp_path):
    run = kernel(command("send", content=[{"type": "text", "text": "look"}, IMAGE]),
                 "-m", "strict/x", env=strict(home, tmp_path, STRICT_SAME="1"), code=1)
    assert len(run.entries_of("replace")) == 1
    assert run.events_of("run_end")[0]["reason"] == "error"


def test_a_torn_last_line_does_not_swallow_the_next_entry(kernel, home):
    first = kernel("one", env=home.env)
    with open(first.session, "a") as f:
        f.write('{"type":"message","id":"torn01","mess')
    kernel("two", env=home.env, session=first.session)
    kernel("three", env=home.env, session=first.session)
    user_texts = [m["content"][0]["text"] if isinstance(m["content"], list) else m["content"]
                  for m in home.requests[-1]["messages"] if m["role"] == "user"]
    assert user_texts == ["one", "two", "three"]
