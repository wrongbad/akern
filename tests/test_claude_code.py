"""The claude-code provider keeps one `claude` running across requests, feeds it only what is new when what it holds is
where the request starts, serves akern's tools to it over its own stdio, and turns its stream into results that stay."""

import json
import os
import subprocess

import pytest

from conftest import BUILD, PATIENCE, REPO

PROVIDER = BUILD / "system" / "providers" / "claude-code"

EXEC = {"name": "exec", "description": "Run a program.", "input_schema": {"type": "object", "properties": {"argv": {"type": "array"}}}}
PNG = {"type": "base64", "media_type": "image/png", "data": "iVBORw0KGgo="}


def text_reply(text, stop="end_turn"):
    return [
        {"type": "message_start", "message": {"usage": {"input_tokens": 5, "cache_read_input_tokens": 7}}},
        {"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}},
        {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": text}},
        {"type": "message_delta", "delta": {"stop_reason": stop}, "usage": {"output_tokens": 3}},
        {"type": "message_stop"}]


TOOL_REPLY = [
    {"type": "message_start", "message": {"usage": {"input_tokens": 5, "cache_read_input_tokens": 7}}},
    {"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}},
    {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": "Looking."}},
    {"type": "content_block_start", "index": 1, "content_block": {"type": "tool_use", "id": "toolu_9", "name": "mcp__akern__exec", "input": {}}},
    {"type": "content_block_delta", "index": 1, "delta": {"type": "input_json_delta", "partial_json": "{\"argv\":[\"ls\"]}"}},
    {"type": "message_delta", "delta": {"stop_reason": "tool_use"}, "usage": {"output_tokens": 3}},
    {"type": "message_stop"}]

ENDS = {"events": text_reply("ok"), "result": {}}


def line(value):
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False)


class Seen:
    def __init__(self, done, log):
        self.done = done
        self.lines = [json.loads(out) for out in done.stdout.splitlines()]
        self.records = [json.loads(r) for r in log.read_text().splitlines()] if log.exists() else []

    @property
    def results(self):
        return [out for out in self.lines if out["type"] == "result"]

    @property
    def starts(self):
        return [r for r in self.records if "start" in r]

    @property
    def stdin(self):
        """The user and assistant lines claude read, across every start."""
        return [r["in"] for r in self.records if "in" in r and r["in"]["type"] in ("user", "assistant")]

    @property
    def mcp(self):
        return [r["mcp"] for r in self.records if "mcp" in r]

    @property
    def argv(self):
        return self.starts[0]["start"]

    @property
    def env(self):
        return self.starts[0]["env"]


@pytest.fixture
def claude(tmp_path):
    def run(*requests, replies=(ENDS,), host_env=None):
        """Sends each request as a line, in order, then closes stdin; requests are message lists or full requests."""
        (tmp_path / "replies.json").write_text(json.dumps(list(replies)))
        log = tmp_path / "log.jsonl"
        env = {**os.environ, **(host_env or {}), "PATH": f"{REPO / 'tests' / 'bin'}{os.pathsep}{os.environ['PATH']}",
               "FAKE_CLAUDE_LOG": str(log), "FAKE_CLAUDE_REPLY": str(tmp_path / "replies.json")}
        lines = [line(r if isinstance(r, dict) else request(r)) + "\n" for r in requests]
        done = subprocess.run([PROVIDER], input="".join(lines), env=env, capture_output=True, text=True, timeout=PATIENCE)
        return Seen(done, log)
    return run


def request(messages, **params):
    return {"model": "haiku", "system": "be brief", "messages": messages, "tools": [EXEC], "params": params}


def user(*blocks):
    return {"role": "user", "content": [b if isinstance(b, dict) else {"type": "text", "text": b} for b in blocks]}


def reply_of(seen, i=0):
    """The assistant message a result carried, as the kernel stores and resends it."""
    return json.loads(line(seen.results[i]["message"]))


def test_history_goes_in_as_stream_json_turns_with_only_the_last_one_queried(claude):
    messages = [
        user("go"),
        {"role": "assistant", "content": [{"type": "tool_use", "id": "tu_1", "name": "exec", "input": {"argv": ["sleep"]}}]},
        {"role": "user", "content": [{"type": "tool_pending", "tool_use_id": "tu_1", "name": "exec", "state": "waiting"}]},
        {"role": "user", "content": [{"type": "job_result", "tool_use_id": "tu_1", "name": "exec", "is_error": False,
                                      "status": "exit 0", "ran": 2, "content": [{"type": "text", "text": "{}"}]}]},
    ]
    seen = claude(request(messages, effort="low", max_tokens=900))
    assert [(line["type"], line.get("shouldQuery")) for line in seen.stdin] == [
        ("user", False), ("assistant", None), ("user", None)]
    assert seen.stdin[1]["message"]["content"][0]["name"] == "mcp__akern__exec"
    assert seen.stdin[2]["message"]["content"] == [
        {"type": "tool_result", "tool_use_id": "tu_1",
         "content": [{"type": "text", "text": "Not finished yet; its result arrives later as a message."}]},
        {"type": "text", "text": '<job call="tu_1" name="exec" status="exit 0" ran="2s">\n{}\n</job>'}]
    argv = seen.argv
    assert argv[argv.index("--tools") + 1] == "" and argv[argv.index("--permission-mode") + 1] == "dontAsk"
    assert [argv[argv.index(flag) + 1] for flag in ("--model", "--system-prompt", "--effort", "--allowedTools")] == [
        "haiku", "be brief", "low", "mcp__akern__exec"]
    assert "--max-turns" not in argv
    assert json.loads(argv[argv.index("--mcp-config") + 1]) == {"mcpServers": {"akern": {"type": "sdk", "name": "akern"}}}
    assert seen.env["CLAUDE_CODE_MAX_OUTPUT_TOKENS"] == "900"
    [(asked, answered)] = [m for m in seen.mcp if m[0]["method"] == "tools/list"]
    assert answered["response"]["mcp_response"]["result"]["tools"] == [
        {"name": "exec", "description": "Run a program.", "inputSchema": EXEC["input_schema"]}]


def test_the_stream_becomes_akern_events_and_a_tool_use_result_that_stays(claude):
    seen = claude([user("list files")], replies=[{"events": TOOL_REPLY}])
    assert seen.done.returncode == 0, seen.done.stdout + seen.done.stderr
    assert seen.lines[:-1] == [
        {"type": "text_delta", "text": "Looking."},
        {"type": "tool_use_start", "id": "toolu_9", "name": "exec"},
        {"type": "tool_use_delta", "id": "toolu_9", "json": "{\"argv\":[\"ls\"]}"}]
    assert seen.lines[-1] == {"type": "result", "stop": "tool_use", "message": {"role": "assistant", "content": [
        {"type": "text", "text": "Looking."},
        {"type": "tool_use", "id": "toolu_9", "name": "exec", "input": {"argv": ["ls"]}}]},
        "usage": {"input": 5, "output": 3, "cache_read": 7, "cache_write": 0}, "stay": True}


def test_the_next_request_answers_the_held_call_in_the_same_claude_without_resending_history(claude, tmp_path):
    first = [user("list files")]
    seen = claude(first, replies=[{"events": TOOL_REPLY, "calls": ["toolu_9"]}])
    result = {"type": "tool_result", "tool_use_id": "toolu_9", "is_error": True,
              "content": [{"type": "text", "text": "a\nb"}, {"type": "image", "source": PNG}]}
    (tmp_path / "replies.json.n").unlink()
    (tmp_path / "log.jsonl").unlink()
    seen = claude(first, first + [reply_of(seen), user(result)],
                  replies=[{"events": TOOL_REPLY, "calls": ["toolu_9"]}, {"events": text_reply("two files"), "result": {}}])
    assert len(seen.starts) == 1
    assert [line["message"]["content"][0]["text"] for line in seen.stdin] == ["list files"]
    [(call, answer)] = [m for m in seen.mcp if m[0]["method"] == "tools/call"]
    assert answer["response"]["mcp_response"]["result"] == {"isError": True, "content": [
        {"type": "text", "text": "a\nb"}, {"type": "image", "data": PNG["data"], "mimeType": "image/png"}]}
    assert [r["stop"] for r in seen.results] == ["tool_use", "end_turn"]
    assert seen.results[1]["message"]["content"] == [{"type": "text", "text": "two files"}]


def test_a_prompt_after_an_answer_goes_to_the_same_claude_as_one_more_line(claude, tmp_path):
    first = [user("hi")]
    seen = claude(first, replies=[ENDS])
    (tmp_path / "replies.json.n").unlink()
    (tmp_path / "log.jsonl").unlink()
    seen = claude(first, first + [reply_of(seen), user("again")], replies=[ENDS, {"events": text_reply("again too"), "result": {}}])
    assert len(seen.starts) == 1
    assert [(line["message"]["content"][0]["text"], line.get("shouldQuery")) for line in seen.stdin] == [
        ("hi", None), ("again", None)]
    assert seen.results[1]["message"]["content"] == [{"type": "text", "text": "again too"}]


def test_a_history_that_is_not_what_claude_holds_starts_claude_again_with_all_of_it(claude, tmp_path):
    seen = claude([user("hi")], replies=[ENDS])
    answer = reply_of(seen)
    (tmp_path / "replies.json.n").unlink()
    (tmp_path / "log.jsonl").unlink()
    seen = claude([user("hi")], [user("edited"), answer, user("again")], replies=[ENDS, ENDS])
    assert len(seen.starts) == 2
    assert [line["message"]["content"][0].get("text") for line in seen.stdin] == ["hi", "edited", "ok", "again"]


def test_results_that_come_with_other_blocks_start_claude_again(claude, tmp_path):
    first = [user("list files")]
    seen = claude(first, replies=[{"events": TOOL_REPLY, "calls": ["toolu_9"]}])
    (tmp_path / "replies.json.n").unlink()
    (tmp_path / "log.jsonl").unlink()
    result = {"type": "tool_result", "tool_use_id": "toolu_9", "is_error": False, "content": [{"type": "text", "text": "a"}]}
    seen = claude(first, first + [reply_of(seen), user(result, "and stop after this")],
                  replies=[{"events": TOOL_REPLY, "calls": ["toolu_9"]}, ENDS])
    assert len(seen.starts) == 2
    assert seen.stdin[-1]["message"]["content"] == [
        {"type": "tool_result", "tool_use_id": "toolu_9", "is_error": False, "content": [{"type": "text", "text": "a"}]},
        {"type": "text", "text": "and stop after this"}]


def test_a_failed_turn_is_an_error_of_the_kind_its_status_names(claude):
    seen = claude([user("hi")], replies=[{"events": [], "result": {"is_error": True, "api_error_status": 429,
                                                                   "result": "usage limit reached"}}])
    assert seen.done.returncode == 1
    assert seen.lines == [{"type": "error", "kind": "rate_limit", "message": "usage limit reached", "status": 429}]


def test_unparsable_tool_input_from_the_model_comes_back_as_a_string_in_the_tool_use(claude):
    deltas = ["", "{\"argv\": s", "h", ", \"argv\": s", "h", "}"]
    seen = claude([user("what does this repo do?")], replies=[{"events": [
        {"type": "message_start", "message": {"usage": {"input_tokens": 2}}},
        {"type": "content_block_start", "index": 0,
         "content_block": {"type": "tool_use", "id": "toolu_1", "name": "mcp__akern__exec", "input": {}}},
        *[{"type": "content_block_delta", "index": 0, "delta": {"type": "input_json_delta", "partial_json": d}}
          for d in deltas],
        {"type": "content_block_stop", "index": 0},
        {"type": "message_delta", "delta": {"stop_reason": "tool_use"}, "usage": {"output_tokens": 78}},
        {"type": "message_stop"}]}])
    assert seen.done.returncode == 0, seen.done.stdout + seen.done.stderr
    assert seen.results[0]["message"]["content"] == [
        {"type": "tool_use", "id": "toolu_1", "name": "exec", "input": "{\"argv\": sh, \"argv\": sh}"}]


def test_a_tool_use_whose_input_is_not_an_object_goes_back_with_an_empty_one(claude):
    seen = claude([
        user("go"),
        {"role": "assistant", "content": [{"type": "tool_use", "id": "tu_1", "name": "exec", "input": "{\"argv\": sh}"}]},
        {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "tu_1", "name": "exec", "is_error": True,
                                      "content": [{"type": "text", "text": "input is not a JSON object: {\"argv\": sh}"}]}]}])
    assert seen.stdin[1]["message"]["content"] == [
        {"type": "tool_use", "id": "tu_1", "name": "mcp__akern__exec", "input": {}}]


def test_claude_runs_without_the_host_sessions_claude_variables(claude):
    seen = claude([user("hi")], host_env={
        "CLAUDECODE": "1", "CLAUDE_CODE_ENTRYPOINT": "claude-desktop", "CLAUDE_CODE_SESSION_ID": "s1",
        "CLAUDE_AGENT_SDK_VERSION": "1", "CLAUDE_CONFIG_DIR": "/cfg", "CLAUDE_CODE_OAUTH_TOKEN": "t"})
    claude_vars = {k: v for k, v in seen.env.items() if k.startswith("CLAUDE")}
    assert claude_vars == {"CLAUDE_CONFIG_DIR": "/cfg", "CLAUDE_CODE_OAUTH_TOKEN": "t", "CLAUDE_CODE_DISABLE_CLAUDE_MDS": "1",
                           "CLAUDE_CODE_DISABLE_AUTO_MEMORY": "1", "CLAUDE_CODE_DISABLE_ADVISOR_TOOL": "1"}
    assert (seen.env["MCP_TOOL_TIMEOUT"], seen.env["MAX_MCP_OUTPUT_TOKENS"]) == ("2147483647", "1000000000")
    assert seen.env["HOME"] == os.environ["HOME"]


def test_models_lists_the_aliases_claude_documents_without_running_it(tmp_path):
    log = tmp_path / "log.json"
    done = subprocess.run([PROVIDER, "--models"], env={"PATH": f"{REPO / 'tests' / 'bin'}:/usr/bin:/bin",
                          "FAKE_CLAUDE_LOG": str(log)}, capture_output=True, text=True, timeout=PATIENCE)
    assert (done.returncode, done.stderr) == (0, "")
    assert [json.loads(line) for line in done.stdout.splitlines()] == [
        {"id": "fable"}, {"id": "sonnet"}, {"id": "opus"}, {"id": "haiku"}]
    assert not log.exists()


def test_models_without_claude_on_path_is_an_error(tmp_path):
    done = subprocess.run([PROVIDER, "--models"], env={"PATH": str(tmp_path)}, capture_output=True, text=True, timeout=PATIENCE)
    assert done.returncode == 1
    assert json.loads(done.stdout) == {"type": "error", "kind": "other", "message": "claude is not on PATH", "status": 0}
