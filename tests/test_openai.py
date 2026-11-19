"""The OpenAI provider renders the kernel's canonical messages as Responses API input items and reads the stream back."""

import json
import os
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

PROVIDER = Path(__file__).resolve().parent.parent / "build" / "providers" / "openai"


def sse(*events):
    return "".join(f"event: {e['type']}\ndata: {json.dumps(e)}\n\n" for e in events).encode()


USAGE = {"input_tokens": 100, "input_tokens_details": {"cached_tokens": 60, "cache_write_tokens": 10},
         "output_tokens": 7, "output_tokens_details": {"reasoning_tokens": 3}, "total_tokens": 107}


def response(*events, final="response.completed", **response):
    return sse({"type": "response.created", "response": {"status": "in_progress"}}, *events,
               {"type": final, "response": {"usage": USAGE, **response}})


def added(index, item):
    return {"type": "response.output_item.added", "output_index": index, "item": item}


def done(index, item):
    return {"type": "response.output_item.done", "output_index": index, "item": item}


REASONING = {"type": "reasoning", "id": "rs_1", "summary": [{"type": "summary_text", "text": "Thinking."}],
             "encrypted_content": "gAAAA-sealed"}


def said(text, index=0, phase="final_answer"):
    message = {"type": "message", "id": "msg_1", "role": "assistant", "phase": phase}
    return [added(index, {**message, "content": []}),
            {"type": "response.output_text.delta", "output_index": index, "content_index": 0, "delta": text},
            done(index, {**message, "status": "completed", "content": [{"type": "output_text", "text": text, "annotations": []}]})]


def called(index, arguments, call_id="call_1", deltas=None):
    call = {"type": "function_call", "id": "fc_1", "call_id": call_id, "name": "exec"}
    return [added(index, {**call, "arguments": ""}),
            *[{"type": "response.function_call_arguments.delta", "output_index": index, "item_id": "fc_1", "delta": d}
              for d in (deltas or [arguments])],
            done(index, {**call, "arguments": arguments, "status": "completed"})]


def thought(index=0):
    return [added(index, {"type": "reasoning", "id": "rs_1", "summary": []}),
            {"type": "response.reasoning_summary_text.delta", "output_index": index, "summary_index": 0, "delta": "Thinking."},
            done(index, REASONING)]


STREAM = response(*said("ok"))


@pytest.fixture(scope="module")
def api():
    seen, replies = [], []

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            seen.append((self.path, dict(self.headers), self.rfile.read(int(self.headers["content-length"]))))
            status, body = replies.pop(0) if replies else (200, STREAM)
            self.send_response(status)
            self.send_header("content-type", "text/event-stream" if status == 200 else "application/json")
            self.send_header("content-length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            seen.append((self.path, dict(self.headers), b""))
            status, body = replies.pop(0)
            self.send_response(status)
            self.send_header("content-length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    env = {**os.environ, "OPENAI_API_KEY": "test-key", "NO_PROXY": "*", "no_proxy": "*",
           "OPENAI_BASE_URL": f"http://127.0.0.1:{server.server_address[1]}/v1/"}

    def send(messages, *answers, params=None, model="gpt-6.2-sol", system="be brief", tools=None, key=True, args=()):
        seen.clear()
        replies[:] = [a if isinstance(a, tuple) else (200, a) for a in answers]
        request = {"model": model, "system": system, "messages": messages, "tools": tools or [], "params": params or {}}
        run_env = env if key else {k: v for k, v in env.items() if k != "OPENAI_API_KEY"}
        finished = subprocess.run([PROVIDER, *args], input=json.dumps(request), env=run_env, capture_output=True,
                                  text=True, timeout=30)
        lines = [json.loads(line) for line in finished.stdout.splitlines()]
        return Sent(seen[:], lines, finished.returncode)

    yield send
    server.shutdown()


class Sent:
    def __init__(self, seen, lines, returncode):
        self.seen, self.lines, self.returncode = seen, lines, returncode

    @property
    def body(self):
        return json.loads(self.seen[-1][2])

    @property
    def input(self):
        return self.body["input"]

    @property
    def result(self):
        assert self.returncode == 0, self.lines
        assert self.lines[-1]["type"] == "result"
        return self.lines[-1]

    @property
    def error(self):
        assert self.returncode == 1, self.lines
        return self.lines[-1]


def user(*blocks):
    return {"role": "user", "content": list(blocks)}


def text(t):
    return {"type": "text", "text": t}


def calls(*ids):
    return {"role": "assistant", "content": [{"type": "tool_use", "id": i, "name": "exec", "input": {"argv": ["ls"]}} for i in ids]}


def result(id, t):
    return {"type": "tool_result", "tool_use_id": id, "name": "exec", "is_error": False, "content": [text(t)]}


def pending(id, **fields):
    return {"type": "tool_pending", "tool_use_id": id, "name": "exec", **fields}


def late(id, *content):
    return {"type": "job_result", "tool_use_id": id, "name": "exec", "is_error": False,
            "status": "exit 0", "ran": 41, "content": list(content)}


PNG = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNgYGD4DwABBAEAwS2OUAAAAABJRU5ErkJggg=="
IMAGE = {"type": "image", "source": {"type": "base64", "media_type": "image/png", "data": PNG}}
EXEC = {"name": "exec", "description": "Run a program.", "input_schema": {"type": "object", "properties": {"argv": {"type": "array"}}}}


def test_the_request_is_a_stateless_streamed_response_with_reasoning_kept_encrypted(api):
    sent = api([user(text("hi"))], tools=[EXEC], params={"max_tokens": 900000, "effort": "low"})
    path, headers, _ = sent.seen[-1]
    assert path == "/v1/responses"
    assert headers["authorization"] == "Bearer test-key"
    body = sent.body
    assert {k: body[k] for k in ("model", "stream", "store", "include", "instructions", "max_output_tokens", "reasoning")} == {
        "model": "gpt-6.2-sol", "stream": True, "store": False, "include": ["reasoning.encrypted_content"],
        "instructions": "be brief", "max_output_tokens": 900000, "reasoning": {"effort": "low", "summary": "auto"}}
    assert body["tools"] == [{"type": "function", "name": "exec", "description": "Run a program.",
                              "parameters": EXEC["input_schema"], "strict": False}]
    assert body["input"] == [{"role": "user", "content": [{"type": "input_text", "text": "hi"}]}]


def test_without_params_effort_is_high_and_no_output_limit_is_sent(api):
    body = api([user(text("hi"))], system=None, params={"summary": "off"}).body
    assert body["reasoning"] == {"effort": "high"}
    assert not {"max_output_tokens", "instructions", "tools"} & body.keys()


SESSION = [
    user(text("go")),
    calls("tu_1", "tu_2"),
    user(text("steer"), pending("tu_1", state="running", pid=4242, out="/p/tu_1.out", err="/p/tu_1.err"), result("tu_2", "two")),
    {"role": "assistant", "content": [text("waiting")]},
    user(late("tu_1", text('{"stdout":"done"}'), IMAGE)),
]


def test_calls_and_their_answers_become_function_call_items_answers_first(api):
    items = api(SESSION[:3]).input
    assert items == [
        {"role": "user", "content": [{"type": "input_text", "text": "go"}]},
        {"type": "function_call", "call_id": "tu_1", "name": "exec", "arguments": '{"argv": ["ls"]}'},
        {"type": "function_call", "call_id": "tu_2", "name": "exec", "arguments": '{"argv": ["ls"]}'},
        {"type": "function_call_output", "call_id": "tu_1", "output":
            "Still running in the background (pid 4242, its own process group); its output so far is in "
            "/p/tu_1.out and /p/tu_1.err. Its result arrives later as a message."},
        {"type": "function_call_output", "call_id": "tu_2", "output": "two"},
        {"role": "user", "content": [{"type": "input_text", "text": "steer"}]},
    ]


def test_a_late_result_is_a_user_message_with_its_media_beside_it(api):
    items = api(SESSION).input
    assert items[6] == {"role": "assistant", "content": "waiting"}
    assert items[7] == {"role": "user", "content": [
        {"type": "input_text", "text": '<job call="tu_1" name="exec" status="exit 0" ran="41s">\n{"stdout":"done"}\n</job>'},
        {"type": "input_image", "image_url": f"data:image/png;base64,{PNG}"}]}


def test_a_tool_result_with_media_is_sent_as_a_list_of_parts(api):
    answer = dict(result("tu_1", "see"), content=[IMAGE, text("see")])
    items = api([user(text("go")), calls("tu_1"), user(answer)]).input
    assert items[2] == {"type": "function_call_output", "call_id": "tu_1", "output": [
        {"type": "input_image", "image_url": f"data:image/png;base64,{PNG}"}, {"type": "input_text", "text": "see"}]}


def test_documents_are_files_named_as_the_block_names_them_and_any_media_type_is_sent_as_named(api):
    pdf = {"type": "document", "source": {"type": "base64", "media_type": "application/pdf", "data": "JVBERi0="}}
    heic = {"type": "image", "source": {"type": "base64", "media_type": "image/heic", "data": "AAAA"}}
    [message] = api([user(text("look"), dict(pdf, name="report.pdf"), pdf, heic)]).input
    assert message["content"][1:] == [
        {"type": "input_file", "filename": "report.pdf", "file_data": "data:application/pdf;base64,JVBERi0="},
        {"type": "input_file", "filename": "document", "file_data": "data:application/pdf;base64,JVBERi0="},
        {"type": "input_image", "image_url": "data:image/heic;base64,AAAA"}]


NESTED = [user(text("go"), IMAGE), calls("tu_1", "tu_2"),
          user(text("look"), result("tu_1", "one"), dict(result("tu_2", "see"), content=[text("see"), IMAGE]),
               late("tu_0", text("{}"), IMAGE))]


@pytest.mark.parametrize("param,at", [
    ("input[0].content[1].image_url", {"message": 0, "block": 1}),
    ("input[4].output[1]", {"message": 2, "block": 2, "inner": 1}),
    ("input[5].content[2].image_url", {"message": 2, "block": 3, "inner": 1}),
])
def test_an_error_whose_param_names_a_media_part_names_the_block_in_the_request(api, param, at):
    message = f"Invalid '{param}'. Expected a base64-encoded data URL with an image MIME type."
    body = json.dumps({"error": {"message": message, "type": "invalid_request_error", "param": param, "code": "invalid_value"}})
    assert api(NESTED, (400, body.encode())).error == {
        "type": "error", "kind": "invalid_block", "message": message, "status": 400, "at": at}


@pytest.mark.parametrize("param", ["input[5].content[0]", "input[1]", "tools[0].name", None])
def test_an_error_whose_param_names_no_media_part_is_an_ordinary_invalid_request(api, param):
    body = json.dumps({"error": {"message": "bad", "type": "invalid_request_error", "param": param, "code": "invalid_value"}})
    assert api(NESTED, (400, body.encode())).error["kind"] == "invalid_request"


def test_extension_blocks_of_other_providers_are_not_sent(api):
    items = api([user(text("go"), {"type": "x-anthropic-note"})]).input
    assert items == [{"role": "user", "content": [{"type": "input_text", "text": "go"}]}]


def test_a_tool_use_whose_input_is_not_an_object_is_sent_with_empty_arguments(api):
    call = {"role": "assistant", "content": [{"type": "tool_use", "id": "tu_1", "name": "exec", "input": "{\"argv\": sh}"}]}
    items = api([user(text("go")), call, user(result("tu_1", "input is not a JSON object"))]).input
    assert items[1] == {"type": "function_call", "call_id": "tu_1", "name": "exec", "arguments": "{}"}


def test_reasoning_and_phases_replay_as_received_and_other_providers_thinking_is_dropped(api):
    reply = {"role": "assistant", "content": [
        {"type": "thinking", "thinking": "", "signature": "anthropic"},
        {"type": "x-openai", "item": REASONING},
        {"type": "text", "text": "Checking.", "x-openai-phase": "commentary"},
        {"type": "tool_use", "id": "call_1", "name": "exec", "input": {}}]}
    items = api([user(text("go")), reply, user(result("call_1", "done"))]).input
    assert items[1:4] == [REASONING, {"role": "assistant", "content": "Checking.", "phase": "commentary"},
                          {"type": "function_call", "call_id": "call_1", "name": "exec", "arguments": "{}"}]


def test_the_stream_becomes_akern_events_and_a_result_that_replays(api):
    sent = api([user(text("go"))], response(*thought(0), *said("Looking.", 1, "commentary"),
                                            *called(2, '{"argv":["ls"]}', deltas=['{"argv":', '["ls"]}'])))
    assert sent.lines[:-1] == [
        {"type": "thinking_delta", "text": "Thinking."},
        {"type": "text_delta", "text": "Looking."},
        {"type": "tool_use_start", "id": "call_1", "name": "exec"},
        {"type": "tool_use_delta", "id": "call_1", "json": '{"argv":'},
        {"type": "tool_use_delta", "id": "call_1", "json": '["ls"]}'}]
    assert sent.result == {"type": "result", "stop": "tool_use", "message": {"role": "assistant", "content": [
        {"type": "x-openai", "item": REASONING},
        {"type": "text", "text": "Looking.", "x-openai-phase": "commentary"},
        {"type": "tool_use", "id": "call_1", "name": "exec", "input": {"argv": ["ls"]}}]},
        "usage": {"input": 30, "output": 7, "cache_read": 60, "cache_write": 10}}


def test_unparsable_arguments_come_back_as_a_string_in_the_tool_use(api):
    sent = api([user(text("go"))], response(*called(0, '{"argv": sh, "argv": sh}')))
    assert sent.result["message"]["content"] == [
        {"type": "tool_use", "id": "call_1", "name": "exec", "input": '{"argv": sh, "argv": sh}'}]


def test_a_response_cut_at_the_output_limit_keeps_its_text_but_not_its_calls_or_unanswered_reasoning(api):
    sent = api([user(text("go"))], response(*said("Partial."), *thought(1), *called(2, '{"argv":'),
                                            final="response.incomplete", incomplete_details={"reason": "max_output_tokens"}))
    assert sent.result["stop"] == "max_tokens"
    assert sent.result["message"]["content"] == [{"type": "text", "text": "Partial.", "x-openai-phase": "final_answer"}]


def test_a_refusal_is_text_with_stop_refusal(api):
    message = {"type": "message", "id": "msg_1", "role": "assistant"}
    sent = api([user(text("go"))], response(
        added(0, {**message, "content": []}),
        {"type": "response.refusal.delta", "output_index": 0, "content_index": 0, "delta": "No."},
        done(0, {**message, "content": [{"type": "refusal", "refusal": "No."}]})))
    assert sent.lines[0] == {"type": "text_delta", "text": "No."}
    assert (sent.result["stop"], sent.result["message"]["content"]) == ("refusal", [text("No.")])


def error_body(code, type="invalid_request_error", message="it failed"):
    return json.dumps({"error": {"message": message, "type": type, "param": None, "code": code}}).encode()


@pytest.mark.parametrize("status,code,type,kind", [
    (401, "invalid_api_key", "invalid_request_error", "auth"),
    (400, "context_length_exceeded", "invalid_request_error", "context_overflow"),
    (429, "insufficient_quota", "insufficient_quota", "other"),
    (400, None, "invalid_request_error", "invalid_request"),
])
def test_an_error_response_is_an_error_line_of_its_kind_without_retries(api, status, code, type, kind):
    sent = api([user(text("go"))], (status, error_body(code, type)))
    assert sent.error == {"type": "error", "kind": kind, "message": "it failed", "status": status}
    assert len(sent.seen) == 1


def test_a_server_error_is_retried(api):
    sent = api([user(text("go"))], (500, error_body("server_error", "server_error")), STREAM)
    assert len(sent.seen) == 2
    assert sent.result["message"]["content"] == [{"type": "text", "text": "ok", "x-openai-phase": "final_answer"}]


def test_a_failure_inside_the_stream_after_output_is_not_retried(api):
    sent = api([user(text("go"))], sse({"type": "response.created", "response": {}}, *said("Hal"),
                                       {"type": "error", "code": "rate_limit_exceeded", "message": "slow down"}))
    assert sent.lines[0] == {"type": "text_delta", "text": "Hal"}
    assert sent.error == {"type": "error", "kind": "rate_limit", "message": "slow down", "status": 200}
    assert len(sent.seen) == 1


def test_a_failed_response_reports_its_error(api):
    sent = api([user(text("go"))], sse({"type": "response.created", "response": {}}, {"type": "response.failed",
               "response": {"status": "failed", "error": {"code": "invalid_prompt", "message": "flagged"}}}))
    assert sent.error == {"type": "error", "kind": "other", "message": "flagged", "status": 200}


def test_a_missing_key_is_an_auth_error(api):
    assert api([user(text("go"))], key=False).error == {
        "type": "error", "kind": "auth", "message": "OPENAI_API_KEY is not set", "status": 0}


def test_models_are_the_ones_the_api_lists(api):
    listed = {"object": "list", "data": [{"id": "gpt-6.2-sol", "object": "model", "owned_by": "openai"},
                                         {"id": "gpt-6-luna", "object": "model", "owned_by": "openai"}]}
    sent = api([], (200, json.dumps(listed).encode()), args=["--models"])
    assert (sent.seen[0][0], sent.seen[0][1]["authorization"]) == ("/v1/models", "Bearer test-key")
    assert sent.lines == [{"id": "gpt-6.2-sol"}, {"id": "gpt-6-luna"}]
    assert api([], (401, error_body("invalid_api_key")), args=["--models"]).error["kind"] == "auth"
