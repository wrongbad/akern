"""Ids the kernel gives out ahead of the entries they name: each inbox item's, and each turn's
assistant message's; and what an abort hands back."""

import json
import re

from conftest import PATIENCE, seen, send_now, tool

HEX8 = re.compile(r"[0-9a-f]{8}")


def send(content, ref, when="turn"):
    return json.dumps({"type": "send", "content": content, "when": when, "ref": ref})


def abort(ref):
    return json.dumps({"type": "abort", "ref": ref})


def slow_reply(*texts):
    """A mock script: a first reply that streams some text, then waits to be interrupted, and a
    reply for each text after it."""
    first = [{"type": "text_delta", "text": "partial "}, {"x-sleep": PATIENCE}]
    rest = [line for text in texts for line in (
        {"type": "text_delta", "text": text},
        {"type": "result", "stop": "end_turn", "message": {"role": "assistant", "content": [{"type": "text", "text": text}]}})]
    return first + rest


def test_a_send_and_an_abort_in_one_write_start_no_run_and_hand_the_prompt_back(kernel):
    run = kernel(send("hi", 1) + "\n" + abort(2))
    ok, queued, returned, aborted = [e for e in run.events if e["type"] in ("ok", "queued", "returned")]
    assert (ok["ref"], queued["item"], aborted) == (1, ok["item"], {"type": "ok", "ref": 2})
    assert returned == {"type": "returned", "returned": ["hi"], "items": [ok["item"]]}
    assert not run.events_of("run_start") and not run.messages


def test_an_abort_with_nothing_queued_sends_no_returned_event(kernel):
    run = kernel(abort(1))
    assert not run.events_of("returned")
    assert run.events_of("ok") == [{"type": "ok", "ref": 1}]


def test_an_idle_abort_keeps_now_items_and_a_send_after_it(kernel):
    run = kernel("\n".join([send("later", 1), send("at once", 2, when="now"), abort(3), send("after", 4)]))
    [returned] = run.events_of("returned")
    assert returned["returned"] == ["later"]
    [delivered] = [e for e in run.events_of("message") if e["meta"]["source"] == "inbox"]
    assert [b["text"] for b in delivered["message"]["content"]] == ["at once", "after"]


def test_each_sent_item_has_an_id_that_its_message_and_entry_name(kernel):
    run = kernel(send("hello", "a"))
    [ok], [queued] = run.events_of("ok"), run.events_of("queued")
    assert HEX8.fullmatch(ok["item"]) and queued["item"] == ok["item"]
    [message] = [e for e in run.events_of("message") if e["meta"]["source"] == "inbox"]
    item = {"id": ok["item"], "source": "prompt", "at": 0, "blocks": 1}
    assert message["meta"] == {"run": 1, "source": "inbox", "items": [item]}
    [entry] = [e for e in run.entries_of("message") if e["message"]["role"] == "user"]
    assert entry == message
    assert ok["item"] not in {e.get("id") for e in run.entries}


def test_a_turns_id_is_the_id_its_assistant_message_gets(kernel):
    run = kernel("hello")
    [start] = run.events_of("turn_start")
    [reply] = [e for e in run.events_of("message") if e["meta"]["source"] == "assistant"]
    assert HEX8.fullmatch(start["id"]) and reply["id"] == start["id"]
    assert run.entries_of("message")[-1]["id"] == start["id"]


def test_a_preempted_turn_keeps_its_id_for_the_partial_reply(kernel, script):
    run = kernel("start", later=[(seen("text_delta"), send_now("stop that"))], env=script(*slow_reply("done")))
    first, second = run.events_of("turn_start")
    partial, final = [e for e in run.entries_of("message") if e["message"]["role"] == "assistant"]
    assert (partial["id"], partial["meta"]["stop"]) == (first["id"], "aborted")
    assert final["id"] == second["id"] != first["id"]


def test_a_turn_aborted_before_any_text_never_uses_its_id(kernel, script):
    env = script({"x-sleep": PATIENCE})
    run = kernel("start", later=[(seen("turn_start"), abort(1))], env=env)
    [start] = run.events_of("turn_start")
    assert start["id"] not in {e["id"] for e in run.entries_of("message")}
    assert run.events_of("run_end")[-1]["reason"] == "aborted"


def test_an_abort_mid_run_hands_queued_prompts_back_in_run_end_with_their_ids(kernel, script):
    run = kernel("start", later=[(seen("text_delta"), send("queued", "q")), (seen("queued", 2), abort("a"))],
                 env=script(*slow_reply()))
    [ok] = [e for e in run.events_of("ok") if e["ref"] == "q"]
    [end] = run.events_of("run_end")
    assert (end["reason"], end["returned"], end["items"]) == ("aborted", ["queued"], [ok["item"]])
    assert not run.events_of("returned")


def test_a_job_result_is_an_item_its_message_names(kernel):
    run = kernel(tool("shell", command="sleep 0.2 && echo bg", wait=0))
    [queued] = [e for e in run.events_of("queued") if e["source"] == "job"]
    [message] = [e for e in run.events_of("message") if queued["item"] in [i["id"] for i in e["meta"].get("items", [])]]
    assert message["message"]["content"][0]["type"] == "tool_result"
    [entry] = [e for e in run.entries_of("message") if e["id"] == message["id"]]
    assert entry["meta"]["items"] == [{"id": queued["item"], "source": "job", "at": 0, "blocks": 1}]


def test_items_delivered_together_name_where_each_ones_blocks_are(kernel):
    image = {"type": "image", "source": {"type": "base64", "media_type": "image/png", "data": "AAAA"}}
    two_blocks = json.dumps({"type": "send", "content": [{"type": "text", "text": "look"}, image], "ref": "b"})
    run = kernel(send("first", "a") + "\n" + two_blocks)
    first, second = ([e["item"] for e in run.events_of("ok") if e["ref"] == ref][0] for ref in ("a", "b"))
    [message] = [e for e in run.events_of("message") if e["meta"]["source"] == "inbox"]
    assert [b["type"] for b in message["message"]["content"]] == ["text", "text", "image"]
    assert message["meta"]["items"] == [{"id": first, "source": "prompt", "at": 0, "blocks": 1},
                                {"id": second, "source": "prompt", "at": 1, "blocks": 2}]
