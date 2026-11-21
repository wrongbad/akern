"""The Anthropic provider renders the kernel's canonical messages into what the Messages API accepts."""

import json
import os
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pytest

from conftest import BUILD, PATIENCE

PROVIDER = BUILD / "system" / "providers" / "anthropic"


def sse(*events):
    return "".join(f"data: {json.dumps(event)}\n\n" for event in events).encode()


STREAM = sse(
    {"type": "message_start", "message": {"usage": {"input_tokens": 1}}},
    {"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}},
    {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": "ok"}},
    {"type": "message_delta", "delta": {"stop_reason": "end_turn"}, "usage": {"output_tokens": 1}},
    {"type": "message_stop"},
)

PNG = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNgYGD4DwABBAEAwS2OUAAAAABJRU5ErkJggg=="
IMAGE = {"type": "image", "source": {"type": "base64", "media_type": "image/png", "data": PNG}}


@pytest.fixture(scope="module")
def api():
    seen, replies = [], []

    class Handler(BaseHTTPRequestHandler):
        def answer(self, request_body):
            seen.append((self.path, dict(self.headers), request_body))
            status, body = replies.pop(0) if replies else (200, STREAM)
            self.send_response(status)
            self.send_header("content-type", "text/event-stream" if status == 200 else "application/json")
            self.send_header("content-length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            self.answer(self.rfile.read(int(self.headers["content-length"])))

        def do_GET(self):
            self.answer(b"")

        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    env = {**os.environ, "ANTHROPIC_API_KEY": "test", "NO_PROXY": "*", "no_proxy": "*",
           "ANTHROPIC_BASE_URL": f"http://127.0.0.1:{server.server_address[1]}"}

    def run(messages, *answers, params=None, args=()):
        seen.clear()
        replies[:] = [a if isinstance(a, tuple) else (200, a) for a in answers]
        request = {"model": "claude-opus-5-5", "system": "", "messages": [], "tools": [], "params": params or {}}
        raw = messages if isinstance(messages, str) else json.dumps(messages)
        stdin = json.dumps(request).replace('"messages": []', '"messages": ' + raw)
        done = subprocess.run([PROVIDER, *args], input=stdin, env=env, capture_output=True, text=True, timeout=PATIENCE)
        return done.returncode, [json.loads(line) for line in done.stdout.splitlines()], seen[:]

    def send(messages, stream=STREAM, params=None):
        code, lines, requests = run(messages, stream, params=params)
        assert code == 0, lines
        assert lines[-1]["type"] == "result"
        return requests[-1][2], lines[-1]

    send.run, send.seen, send.replies = run, seen, replies
    send.env = {k: env[k] for k in ("ANTHROPIC_API_KEY", "ANTHROPIC_BASE_URL", "NO_PROXY", "no_proxy")}
    yield send
    server.shutdown()


def sent(api, messages):
    return json.loads(api(messages)[0])["messages"]


def user(*blocks):
    return {"role": "user", "content": list(blocks)}


def text(t):
    return {"type": "text", "text": t}


def calls(*ids):
    return {"role": "assistant", "content": [{"type": "tool_use", "id": i, "name": "exec", "input": {}} for i in ids]}


def result(id, t, **fields):
    return {"type": "tool_result", "tool_use_id": id, "name": "exec", "is_error": False, "content": [text(t)], **fields}


def pending(id, **fields):
    return {"type": "tool_pending", "tool_use_id": id, "name": "exec", **fields}


def late(id, *content, status="exit 0", ran=41):
    return {"type": "job_result", "tool_use_id": id, "name": "exec", "is_error": False,
            "status": status, "ran": ran, "content": list(content)}


SESSION = [
    user(text("go")),
    calls("tu_1", "tu_2"),
    user(text("steer"), pending("tu_1", state="running", pid=4242, out="/p/s.jsonl.d/calls/tu_1.out",
                                err="/p/s.jsonl.d/calls/tu_1.err", stdin_open=True), result("tu_2", "two")),
    calls("tu_3"),
    user(late("tu_1", text('{"stdout":'), IMAGE, text('"done"}')), result("tu_3", "three")),
    user(text("closing")),
    user(text("next prompt")),
]


def test_a_pending_call_is_answered_with_where_its_process_and_output_are(api):
    answer = sent(api, SESSION[:3])[2]["content"][0]
    assert answer == {"type": "tool_result", "tool_use_id": "tu_1", "content": [text(
        "Still running in the background (pid 4242, its own process group); its output so far is in "
        "/p/s.jsonl.d/calls/tu_1.out and /p/s.jsonl.d/calls/tu_1.err. Its result arrives later as a message. "
        'Its stdin is open: send it input with the job tool, id "tu_1".')]}


@pytest.mark.parametrize("state", ["waiting", None])
def test_a_pending_call_without_a_live_process_says_only_that_its_result_comes_later(api, state):
    fields = {"state": state} if state else {}
    [answer] = sent(api, [user(text("go")), calls("tu_1"), user(pending("tu_1", **fields))])[2]["content"]
    assert answer["content"] == [text("Not finished yet; its result arrives later as a message.")]


def test_tool_results_come_first_in_their_order_then_the_rest(api):
    blocks = sent(api, SESSION[:3])[2]["content"]
    assert [(b["type"], b.get("tool_use_id")) for b in blocks] == [
        ("tool_result", "tu_1"), ("tool_result", "tu_2"), ("text", None)]


def test_a_late_result_is_a_job_after_the_tool_results_with_its_images_beside_it(api):
    blocks = sent(api, SESSION[:5])[4]["content"]
    assert blocks == [
        {"type": "tool_result", "tool_use_id": "tu_3", "is_error": False, "content": [text("three")]},
        text('<job call="tu_1" name="exec" status="exit 0" ran="41s">\n{"stdout":"done"}\n</job>'),
        IMAGE,
    ]


def test_consecutive_user_messages_are_sent_as_one(api):
    messages = sent(api, SESSION)
    assert [m["role"] for m in messages] == ["user", "assistant", "user", "assistant", "user"]
    assert messages[4]["content"][3:] == [text("closing"), text("next prompt")]


def test_kernel_only_fields_are_not_sent(api):
    for message in sent(api, SESSION):
        for block in message["content"]:
            if block["type"] == "tool_result":
                assert not {"name", "status", "ran", "state"} & block.keys()
            assert block["type"] not in ("tool_pending", "job_result")


def test_an_earlier_message_renders_the_same_whatever_follows(api):
    def ordered(messages):
        return dict(json.loads(api(messages)[0], object_pairs_hook=list))["messages"]

    whole = ordered(SESSION)
    for n in range(1, len(SESSION)):
        part = ordered(SESSION[:n])
        assert part[:-1] == whole[:len(part) - 1]
        last, same = dict(part[-1]), dict(whole[len(part) - 1])
        assert last["role"] == same["role"]
        assert same["content"][:len(last["content"])] == last["content"]


def test_an_image_in_a_late_result_is_still_held_to_the_api_limits(api):
    huge = {"type": "image", "source": {"type": "base64", "media_type": "image/png", "data": "A" * (10 << 20 | 4)}}
    blocks = sent(api, [user(text("go")), calls("tu_1"), user(pending("tu_1")),
                        {"role": "assistant", "content": [text("waiting")]},
                        user(late("tu_1", text("{}"), huge))])[4]["content"]
    assert blocks[1] == text("[image omitted: larger than the API's 10 MB limit. The original is still in the session.]")


def rejected(api, messages, message, status=400, type="invalid_request_error"):
    code, lines, requests = api.run(messages, (status, json.dumps({"type": "error", "error": {"type": type, "message": message}}).encode()))
    assert code == 1 and len(requests) == 1
    return lines[-1]


NESTED = [user(text("a")), user(text("b"), IMAGE), calls("tu_1"),
          user(dict(result("tu_1", "see"), content=[text("see"), IMAGE]))]


@pytest.mark.parametrize("messages,position,at", [
    (SESSION, "messages.4.content.2.image.source.base64.data", {"message": 4, "block": 0, "inner": 1}),
    (NESTED, "messages.0.content.2.image.source.base64", {"message": 1, "block": 1}),
    (NESTED, "messages.2.content.0.content.1.image.source.base64", {"message": 3, "block": 0, "inner": 1}),
    (NESTED, "messages.2.content.0.tool_result.content.1.image.source.base64", {"message": 3, "block": 0, "inner": 1}),
])
def test_an_error_naming_a_media_block_names_it_in_the_request_the_kernel_sent(api, messages, position, at):
    assert rejected(api, messages, f"{position}: image exceeds 5 MB maximum") == {
        "type": "error", "kind": "invalid_block", "message": "image exceeds 5 MB maximum", "status": 400, "at": at}


@pytest.mark.parametrize("message", [
    "messages.1.content.0: Invalid `signature` in `thinking` block.",
    "messages.0.content.0.text: field required",
    "image exceeds 5 MB maximum",
])
def test_an_error_naming_no_media_block_is_an_ordinary_invalid_request(api, message):
    assert rejected(api, NESTED, message) == {"type": "error", "kind": "invalid_request", "message": message, "status": 400}


def test_through_the_kernel_a_rejected_image_is_replaced_and_the_run_goes_on(api, kernel):
    error = {"type": "error", "error": {"type": "invalid_request_error",
             "message": "messages.0.content.1.image.source.base64.data: image dimensions exceed max allowed size: 8000 pixels"}}
    api.seen.clear()
    api.replies[:] = [(400, json.dumps(error).encode())]
    look = json.dumps({"type": "send", "content": [text("look"), IMAGE]})
    run = kernel(look, "-m", "anthropic/claude-opus-5-5", env=api.env)
    [replaced] = run.entries_of("replace")
    note = text("[image omitted: image dimensions exceed max allowed size: 8000 pixels]")
    assert replaced["message"] == user(text("look"), note)
    assert json.loads(api.seen[-1][2])["messages"] == [user(text("look"), note)]
    assert (run.replies, run.events_of("run_end")[0]["reason"]) == (["ok"], "done")


def test_extension_blocks_and_fields_are_not_sent(api):
    reply = {"role": "assistant", "content": [
        {"type": "x-openai", "item": {"type": "reasoning"}}, {"type": "text", "text": "Checking.", "x-openai-phase": "commentary"},
        {"type": "tool_use", "id": "tu_1", "name": "exec", "input": {}, "x-note": 1}]}
    messages = sent(api, [user(text("go"), {"type": "x-mark"}), reply, user(result("tu_1", "done"))])
    assert messages[:2] == [user(text("go")), {"role": "assistant", "content": [
        text("Checking."), {"type": "tool_use", "id": "tu_1", "name": "exec", "input": {}}]}]


def test_without_extensions_a_message_is_sent_byte_for_byte(api):
    messages = ('[{"role":"user","content":[{"type": "text",  "text":"go"}]},'
                '{"role":"assistant","content":[{"type":"text", "text":"a","citations":null} ,{"type":"thinking","thinking":"","signature":"s"}]},'
                '{"role":"user","content":"next"}]')
    body, _ = api(messages)
    assert (b'"messages":[{"role":"user","content":[{"type": "text",  "text":"go"}]},{"role":"assistant","content":['
            b'{"type":"text", "text":"a","citations":null},{"type":"thinking","thinking":"","signature":"s"}]},') in body


def test_a_document_name_is_sent_as_its_title_and_unnamed_media_byte_for_byte(api):
    pdf = '{"type":"document", "source":{"type":"base64","media_type":"application/pdf","data":"JVBERi0="}}'
    named = '{"type":"document","name":"report.pdf","source":{"type":"base64","media_type":"application/pdf","data":"JVBERi0="}}'
    titled = '{"type":"document","title":"report.pdf","source":{"type":"base64","media_type":"application/pdf","data":"JVBERi0="}}'
    use = '{"role":"assistant","content":[{"type":"tool_use","id":"tu_1","name":"read","input":{}}]}'
    result = '{"type":"tool_result","tool_use_id":"tu_1","name":"read","is_error":false,"content":[%s, %s]}'
    body, _ = api(f'[{{"role":"user","content":[{named},{pdf}]}},{use},{{"role":"user","content":[{result % (pdf, pdf)}]}},'
                  f'{use},{{"role":"user","content":[{result % (named, pdf)}]}}]')
    assert f'{{"role":"user","content":[{titled},{pdf}]}}'.encode() in body
    assert f'"content":[{pdf}, {pdf}]}}'.encode() in body
    assert f'"is_error":false,"content":[{titled},{pdf}]}}'.encode() in body


def test_thinking_effort_and_fallbacks_are_on_unless_params_turn_them_off(api):
    body = json.loads(api([user(text("go"))])[0])
    assert (body["max_tokens"], body["thinking"], body["output_config"], body["fallbacks"]) == (
        64000, {"type": "adaptive", "display": "summarized"}, {"effort": "high"}, "default")
    body = json.loads(api([user(text("go"))], params={"thinking": "off", "effort": "off", "fallbacks": "off", "max_tokens": 500000})[0])
    assert body["max_tokens"] == 500000
    assert not {"thinking", "output_config", "fallbacks"} & body.keys()
    body = json.loads(api([user(text("go"))], params={"thinking": {"type": "enabled", "budget_tokens": 900}, "effort": "off"})[0])
    assert body["thinking"] == {"type": "enabled", "budget_tokens": 900}


def test_models_are_the_ones_the_api_lists_page_by_page(api):
    first = {"data": [{"type": "model", "id": "claude-opus-5-5", "max_input_tokens": 1000000, "max_tokens": 128000}],
             "has_more": True, "last_id": "claude-opus-5-5"}
    second = {"data": [{"type": "model", "id": "claude-haiku-4-5", "max_input_tokens": None}], "has_more": False, "last_id": "claude-haiku-4-5"}
    code, lines, requests = api.run([], (200, json.dumps(first).encode()), (200, json.dumps(second).encode()), args=["--models"])
    assert code == 0
    assert [r[0] for r in requests] == ["/v1/models?limit=1000", "/v1/models?limit=1000&after_id=claude-opus-5-5"]
    assert requests[0][1]["x-api-key"] == "test"
    assert lines == [{"id": "claude-opus-5-5", "context": 1000000, "max_output": 128000}, {"id": "claude-haiku-4-5"}]


def test_unparsable_tool_input_from_the_model_comes_back_as_a_string_in_the_tool_use(api):
    _, answer = api([user(text("go"))], stream=sse(
        {"type": "message_start", "message": {"usage": {"input_tokens": 1}}},
        {"type": "content_block_start", "index": 0, "content_block": {"type": "tool_use", "id": "tu_1", "name": "exec", "input": {}}},
        *[{"type": "content_block_delta", "index": 0, "delta": {"type": "input_json_delta", "partial_json": d}}
          for d in ["{\"argv\": s", "h, \"argv\": sh}"]],
        {"type": "content_block_stop", "index": 0},
        {"type": "message_delta", "delta": {"stop_reason": "tool_use"}, "usage": {"output_tokens": 9}},
        {"type": "message_stop"}))
    assert answer["message"]["content"] == [
        {"type": "tool_use", "id": "tu_1", "name": "exec", "input": "{\"argv\": sh, \"argv\": sh}"}]


def test_a_tool_use_whose_input_is_not_an_object_is_sent_with_an_empty_one(api):
    call = {"role": "assistant", "content": [text("trying"), {"type": "tool_use", "id": "tu_1", "name": "exec", "input": "{\"argv\": sh}"}]}
    assert sent(api, [user(text("go")), call, user(result("tu_1", "input is not a JSON object"))])[1] == {
        "role": "assistant", "content": [text("trying"), {"type": "tool_use", "id": "tu_1", "name": "exec", "input": {}}]}
