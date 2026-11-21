"""Invariants every kernel run must keep, in its session log and its event stream."""


def session_violations(entries):
    messages = [e for e in entries if e.get("type") == "message"]
    found = []
    uses, answers = [], []
    for i, entry in enumerate(messages):
        message = entry["message"]
        blocks = _blocks(message)
        calls = [b["id"] for b in blocks if b.get("type") == "tool_use"]
        results = [b["tool_use_id"] for b in blocks if b.get("type") == "tool_result"]
        answers += [r for r in results if r in uses]
        if not calls:
            continue
        if entry.get("meta", {}).get("stop") == "aborted":
            found.append(f"tool_use inside an aborted message: {calls}")
        if message["role"] != "assistant":
            continue
        uses += calls
        following = messages[i + 1]["message"] if i + 1 < len(messages) else {}
        in_turn = [b["tool_use_id"] for b in _blocks(following)
                   if b.get("type") == "tool_result" and "status" not in b]
        if stray := [r for r in in_turn if r not in calls]:
            found.append(f"tool_use {calls} followed by results of other calls {stray}")
    if wrong := [u for u in uses if answers.count(u) != 1]:
        found.append(f"tool_use not answered exactly once: {wrong}")
    return found


def event_violations(events):
    found = []
    started, announced = [], []
    for e in events:
        if e["type"] == "tool_start":
            started.append(e["call"])
        elif e["type"] == "block" and e["block"].get("type") == "tool_result":
            call = e["block"]["tool_use_id"]
            if call not in started:
                found.append(f"block for {call} before its tool_start")
            announced.append(call)
        elif e["type"] == "message" and e["message"].get("role") == "user":
            results = [b["tool_use_id"] for b in _blocks(e["message"]) if b.get("type") == "tool_result"]
            if unannounced := [r for r in results if r in started and r not in announced]:
                found.append(f"message with results no block event announced: {unannounced}")
    if unfinished := [c for c in started if c not in announced]:
        found.append(f"tool_start without a block: {unfinished}")
    streams = [e["type"] for e in events
               if e["type"] in ("turn_start", "run_end", "replace") or e["type"] == "message" and e.get("source") == "assistant"]
    if any(a == b == "turn_start" for a, b in zip(streams, streams[1:])):
        found.append("two provider streams open at once (turn_start twice without an assistant message, replace or run_end between)")
    if disagreeing := [e["run"] for e in events
                       if e["type"] == "run_end" and (e["reason"] == "error") != (e["error"] is not None)]:
        found.append(f"run_end with an error but no error reason, or the reverse, in runs {disagreeing}")
    return found


def _blocks(message):
    content = message.get("content")
    return content if isinstance(content, list) else []
