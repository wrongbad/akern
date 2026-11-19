"""Invariants every kernel run must keep, in its session log and its event stream."""


def session_violations(entries):
    messages = [e for e in entries if e.get("type") == "message"]
    found = []
    pending, late = [], []
    for i, entry in enumerate(messages):
        message = entry["message"]
        blocks = _blocks(message)
        pending += [b["tool_use_id"] for b in blocks if b.get("type") == "tool_pending"]
        late += [b["tool_use_id"] for b in blocks if b.get("type") == "job_result"]
        uses = [b["id"] for b in blocks if b.get("type") == "tool_use"]
        if not uses:
            continue
        if entry.get("meta", {}).get("stop") == "aborted":
            found.append(f"tool_use inside an aborted message: {uses}")
        if message["role"] != "assistant":
            continue
        following = messages[i + 1]["message"] if i + 1 < len(messages) else {}
        answered = [b["tool_use_id"] for b in _blocks(following)
                    if b.get("type") in ("tool_result", "tool_pending")]
        if answered != uses:
            found.append(f"tool_use {uses} answered by {answered} in the next message")
    if sorted(pending) != sorted(late):
        found.append(f"pending calls {pending} closed by late results {late}")
    return found


def event_violations(events):
    found = []
    started = [e["call"] for e in events if e["type"] == "tool_start"]
    finished = {e["call"] for e in events if e["type"] == "tool_end"}
    finished |= {b["tool_use_id"] for e in events if e["type"] == "message"
                 for b in _blocks(e["message"]) if b.get("type") in ("tool_result", "job_result")}
    if unfinished := [c for c in started if c not in finished]:
        found.append(f"tool_start without tool_end or a result: {unfinished}")
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
