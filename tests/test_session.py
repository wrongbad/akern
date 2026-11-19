import json
import shutil

from conftest import seen

PNG = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNgYGD4DwABBAEAwS2OUAAAAABJRU5ErkJggg=="
IMAGE = {"type": "image", "source": {"type": "base64", "media_type": "image/png", "data": PNG}}

STRICT = '''#!/usr/bin/env python3
"""Rejects the first image or document block of a request, as an API that names the block would."""
import json, os, sys
request = json.load(sys.stdin)
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


def test_an_appended_entry_of_type_session_becomes_the_head(kernel):
    run = kernel(json.dumps({"type": "append", "entry": {"type": "session"}, "ref": 1}), later=[(0, "hello")])
    _, appended = run.entries_of("session")
    assert run.events_of("ok") == [{"type": "ok", "id": appended["id"], "ref": 1}]
    assert run.entries_of("message")[0]["parent"] == appended["id"]


def test_the_newest_replace_entry_is_what_later_requests_send_on_every_branch_and_after_a_reload(kernel, home):
    run = kernel("secret", later=[
        (seen("run_end"), lambda run: replace(first_id(run), "first")),
        (0, lambda run: replace(first_id(run), "redacted")),
        (0, lambda run: replace("nope", "ignored")),
        (0, "again"),
        (seen("run_end", 2), lambda run: command("append", entry={"type": "branch", "parent": first_id(run)})),
        (0, "branched")], env=home.env)
    redacted = {"role": "user", "content": [{"type": "text", "text": "redacted"}]}
    assert [r["messages"][0] for r in home.requests] == [
        {"role": "user", "content": [{"type": "text", "text": "secret"}]}, redacted, redacted]
    assert home.requests[2]["messages"][1:] == [{"role": "user", "content": [{"type": "text", "text": "branched"}]}]
    assert run.entries_of("message")[0]["message"]["content"][0]["text"] == "secret"
    shutil.copy(run.session, kernel.dir / f"run{kernel.runs + 1}.jsonl")
    kernel("resumed", env=home.env)
    assert home.requests[-1]["messages"][0] == redacted


def strict(home, tmp_path, **env):
    path = home.dir / "providers" / "strict"
    path.write_text(STRICT)
    path.chmod(0o755)
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
