"""The Anthropic provider renders the kernel's canonical messages into what the Messages API accepts."""

import json
import os
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

PROVIDER = Path(__file__).resolve().parent.parent / "build" / "providers" / "anthropic"

STREAM = "".join(f"data: {json.dumps(event)}\n\n" for event in [
    {"type": "message_start", "message": {"usage": {"input_tokens": 1}}},
    {"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}},
    {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": "ok"}},
    {"type": "message_delta", "delta": {"stop_reason": "end_turn"}, "usage": {"output_tokens": 1}},
    {"type": "message_stop"},
]).encode()

PNG = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNgYGD4DwABBAEAwS2OUAAAAABJRU5ErkJggg=="
IMAGE = {"type": "image", "source": {"type": "base64", "media_type": "image/png", "data": PNG}}


@pytest.fixture(scope="module")
def api():
    bodies = []

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            bodies.append(self.rfile.read(int(self.headers["content-length"])))
            self.send_response(200)
            self.send_header("content-type", "text/event-stream")
            self.send_header("content-length", str(len(STREAM)))
            self.end_headers()
            self.wfile.write(STREAM)

        def log_message(self, *args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    env = {**os.environ, "ANTHROPIC_API_KEY": "test", "NO_PROXY": "*", "no_proxy": "*",
           "ANTHROPIC_BASE_URL": f"http://127.0.0.1:{server.server_address[1]}"}

    def send(messages):
        request = {"model": "claude-opus-5-5", "system": "", "messages": messages, "tools": [], "params": {}}
        done = subprocess.run([PROVIDER], input=json.dumps(request), env=env, capture_output=True, text=True, timeout=30)
        assert done.returncode == 0, done.stdout + done.stderr
        assert json.loads(done.stdout.splitlines()[-1])["type"] == "result"
        return bodies[-1]

    yield send
    server.shutdown()


def sent(api, messages):
    return json.loads(api(messages))["messages"]


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
        'Its stdin is open: send it input with write and job "tu_1".')]}


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
        return dict(json.loads(api(messages), object_pairs_hook=list))["messages"]

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


def test_a_media_type_the_api_does_not_accept_is_replaced_by_a_note(api):
    heic = {"type": "image", "source": {"type": "base64", "media_type": "image/heic", "data": "AAAA"}}
    [message] = sent(api, [user(text("look"), heic)])
    assert message["content"][1] == text(
        "[image omitted: image/heic is not a type the API accepts. The original is still in the session.]")
