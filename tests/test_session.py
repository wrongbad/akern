import json


def test_an_appended_entry_of_type_session_becomes_the_head(kernel):
    run = kernel(json.dumps({"type": "append", "entry": {"type": "session"}, "ref": 1}), later=[(0, "hello")])
    _, appended = run.entries_of("session")
    assert run.events_of("ok") == [{"type": "ok", "id": appended["id"], "ref": 1}]
    assert run.entries_of("message")[0]["parent"] == appended["id"]
