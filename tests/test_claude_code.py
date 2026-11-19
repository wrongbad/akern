"""The claude-code provider turns a request into one `claude -p` turn whose tools are akern's, and its stream back into a result."""

import json
import os
import subprocess

import pytest

from conftest import BUILD, REPO

PROVIDER = BUILD / "system" / "providers" / "claude-code"

EXEC = {"name": "exec", "description": "Run a program.", "input_schema": {"type": "object", "properties": {"argv": {"type": "array"}}}}


def stream(*events, **result):
    lines = [{"type": "system", "subtype": "init"}]
    lines += [{"type": "stream_event", "event": e, "parent_tool_use_id": None} for e in events]
    lines.append({"type": "result", "is_error": False, **result})
    return "".join(json.dumps(line) + "\n" for line in lines)


TOOL_TURN = stream(
    {"type": "message_start", "message": {"usage": {"input_tokens": 5, "cache_read_input_tokens": 7}}},
    {"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}},
    {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": "Looking."}},
    {"type": "content_block_start", "index": 1, "content_block": {"type": "tool_use", "id": "toolu_9", "name": "mcp__akern__exec", "input": {}}},
    {"type": "content_block_delta", "index": 1, "delta": {"type": "input_json_delta", "partial_json": "{\"argv\":[\"ls\"]}"}},
    {"type": "message_delta", "delta": {"stop_reason": "tool_use"}, "usage": {"output_tokens": 3}},
    {"type": "message_stop"},
    subtype="error_max_turns")


@pytest.fixture
def claude(tmp_path):
    def run(messages, reply=TOOL_TURN, params=None, host_env=None):
        (tmp_path / "reply.jsonl").write_text(reply)
        env = {**os.environ, **(host_env or {}), "PATH": f"{REPO / 'tests' / 'bin'}{os.pathsep}{os.environ['PATH']}",
               "FAKE_CLAUDE_LOG": str(tmp_path / "log.json"), "FAKE_CLAUDE_REPLY": str(tmp_path / "reply.jsonl")}
        request = {"model": "haiku", "system": "be brief", "messages": messages, "tools": [EXEC], "params": params or {}}
        done = subprocess.run([PROVIDER], input=json.dumps(request), env=env, capture_output=True, text=True, timeout=30)
        return done, json.loads((tmp_path / "log.json").read_text())
    return run


def test_history_goes_in_as_stream_json_turns_with_only_the_last_one_queried(claude):
    messages = [
        {"role": "user", "content": [{"type": "text", "text": "go"}]},
        {"role": "assistant", "content": [{"type": "tool_use", "id": "tu_1", "name": "exec", "input": {"argv": ["sleep"]}}]},
        {"role": "user", "content": [{"type": "tool_pending", "tool_use_id": "tu_1", "name": "exec", "state": "waiting"}]},
        {"role": "user", "content": [{"type": "job_result", "tool_use_id": "tu_1", "name": "exec", "is_error": False,
                                      "status": "exit 0", "ran": 2, "content": [{"type": "text", "text": "{}"}]}]},
    ]
    _, seen = claude(messages, params={"effort": "low", "max_tokens": 900})
    assert [(line["type"], line.get("shouldQuery")) for line in seen["stdin"]] == [
        ("user", False), ("assistant", None), ("user", None)]
    assert seen["stdin"][1]["message"]["content"][0]["name"] == "mcp__akern__exec"
    assert seen["stdin"][2]["message"]["content"] == [
        {"type": "tool_result", "tool_use_id": "tu_1",
         "content": [{"type": "text", "text": "Not finished yet; its result arrives later as a message."}]},
        {"type": "text", "text": '<job call="tu_1" name="exec" status="exit 0" ran="2s">\n{}\n</job>'}]
    argv = seen["argv"]
    assert argv[argv.index("--tools") + 1] == "" and argv[argv.index("--permission-mode") + 1] == "dontAsk"
    assert [argv[argv.index(flag) + 1] for flag in ("--model", "--system-prompt", "--effort", "--max-turns")] == [
        "haiku", "be brief", "low", "1"]
    assert seen["env"]["CLAUDE_CODE_MAX_OUTPUT_TOKENS"] == "900"
    [server] = json.loads(argv[argv.index("--mcp-config") + 1])["mcpServers"].values()
    tools = subprocess.run([server["command"], *server["args"]], capture_output=True, text=True, timeout=10, input=
                           '{"jsonrpc":"2.0","id":1,"method":"tools/list"}\n').stdout
    assert json.loads(tools)["result"]["tools"] == [
        {"name": "exec", "description": "Run a program.", "inputSchema": EXEC["input_schema"]}]


def test_the_stream_becomes_akern_events_and_a_tool_use_result_named_for_akern(claude):
    done, _ = claude([{"role": "user", "content": "list files"}])
    lines = [json.loads(line) for line in done.stdout.splitlines()]
    assert done.returncode == 0, done.stdout + done.stderr
    assert lines[:-1] == [
        {"type": "text_delta", "text": "Looking."},
        {"type": "tool_use_start", "id": "toolu_9", "name": "exec"},
        {"type": "tool_use_delta", "id": "toolu_9", "json": "{\"argv\":[\"ls\"]}"}]
    assert lines[-1] == {"type": "result", "stop": "tool_use", "message": {"role": "assistant", "content": [
        {"type": "text", "text": "Looking."},
        {"type": "tool_use", "id": "toolu_9", "name": "exec", "input": {"argv": ["ls"]}}]},
        "usage": {"input": 5, "output": 3, "cache_read": 7, "cache_write": 0}}


def test_a_failed_turn_is_an_error_of_the_kind_its_status_names(claude):
    done, _ = claude([{"role": "user", "content": "hi"}],
                     reply=stream(is_error=True, api_error_status=429, result="usage limit reached"))
    assert done.returncode == 1
    assert json.loads(done.stdout) == {"type": "error", "kind": "rate_limit", "message": "usage limit reached", "status": 429}


def test_unparsable_tool_input_from_the_model_comes_back_as_a_string_in_the_tool_use(claude):
    deltas = ["", "{\"argv\": s", "h", ", \"argv\": s", "h", "}"]
    done, _ = claude([{"role": "user", "content": "what does this repo do?"}], reply=stream(
        {"type": "message_start", "message": {"usage": {"input_tokens": 2}}},
        {"type": "content_block_start", "index": 0,
         "content_block": {"type": "tool_use", "id": "toolu_1", "name": "mcp__akern__exec", "input": {}}},
        *[{"type": "content_block_delta", "index": 0, "delta": {"type": "input_json_delta", "partial_json": d}}
          for d in deltas],
        {"type": "content_block_stop", "index": 0},
        {"type": "message_delta", "delta": {"stop_reason": "tool_use"}, "usage": {"output_tokens": 78}},
        {"type": "message_stop"},
        subtype="error_max_turns"))
    assert done.returncode == 0, done.stdout + done.stderr
    assert json.loads(done.stdout.splitlines()[-1])["message"]["content"] == [
        {"type": "tool_use", "id": "toolu_1", "name": "exec", "input": "{\"argv\": sh, \"argv\": sh}"}]


def test_a_tool_use_whose_input_is_not_an_object_goes_back_with_an_empty_one(claude):
    _, seen = claude([
        {"role": "user", "content": [{"type": "text", "text": "go"}]},
        {"role": "assistant", "content": [{"type": "tool_use", "id": "tu_1", "name": "exec", "input": "{\"argv\": sh}"}]},
        {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "tu_1", "name": "exec", "is_error": True,
                                      "content": [{"type": "text", "text": "input is not a JSON object: {\"argv\": sh}"}]}]}])
    assert seen["stdin"][1]["message"]["content"] == [
        {"type": "tool_use", "id": "tu_1", "name": "mcp__akern__exec", "input": {}}]


def test_claude_runs_without_the_host_sessions_claude_variables(claude):
    _, seen = claude([{"role": "user", "content": "hi"}], host_env={
        "CLAUDECODE": "1", "CLAUDE_CODE_ENTRYPOINT": "claude-desktop", "CLAUDE_CODE_SESSION_ID": "s1",
        "CLAUDE_AGENT_SDK_VERSION": "1", "CLAUDE_CONFIG_DIR": "/cfg", "CLAUDE_CODE_OAUTH_TOKEN": "t"})
    claude_vars = {k: v for k, v in seen["env"].items() if k.startswith("CLAUDE")}
    assert claude_vars == {"CLAUDE_CONFIG_DIR": "/cfg", "CLAUDE_CODE_OAUTH_TOKEN": "t", "CLAUDE_CODE_DISABLE_CLAUDE_MDS": "1",
                           "CLAUDE_CODE_DISABLE_AUTO_MEMORY": "1", "CLAUDE_CODE_DISABLE_ADVISOR_TOOL": "1"}
    assert seen["env"]["HOME"] == os.environ["HOME"]


def test_models_lists_the_aliases_claude_documents_without_running_it(tmp_path):
    log = tmp_path / "log.json"
    done = subprocess.run([PROVIDER, "--models"], env={"PATH": f"{REPO / 'tests' / 'bin'}:/usr/bin:/bin",
                          "FAKE_CLAUDE_LOG": str(log)}, capture_output=True, text=True, timeout=10)
    assert (done.returncode, done.stderr) == (0, "")
    assert [json.loads(line) for line in done.stdout.splitlines()] == [
        {"id": "fable"}, {"id": "sonnet"}, {"id": "opus"}, {"id": "haiku"}]
    assert not log.exists()


def test_models_without_claude_on_path_is_an_error(tmp_path):
    done = subprocess.run([PROVIDER, "--models"], env={"PATH": str(tmp_path)}, capture_output=True, text=True, timeout=10)
    assert done.returncode == 1
    assert json.loads(done.stdout) == {"type": "error", "kind": "other", "message": "claude is not on PATH", "status": 0}
