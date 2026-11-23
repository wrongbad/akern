"""akern-acp: an ACP agent in front of akern, driven here as an ACP client would drive it."""

import json
import queue
import subprocess
import threading
import time

import pytest

from conftest import PATIENCE, tool, wait_for


class Client:
    """One akern-acp process, spoken to over JSON-RPC lines."""

    def __init__(self, env, dir, *args):
        self.proc = subprocess.Popen(["akern-acp", "--sessions", str(dir / "sessions"), *args], cwd=dir, env=env,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.lines = queue.Queue()
        self.next_id = 0
        self.updates = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            self.lines.put(json.loads(line))
        self.lines.put(None)

    def send(self, method, params=None, id=True):
        message = {"jsonrpc": "2.0", "method": method, "params": params or {}}
        if id:
            self.next_id += 1
            message["id"] = self.next_id
        self.proc.stdin.write(json.dumps(message) + "\n")
        self.proc.stdin.flush()
        return message.get("id")

    def wait(self, id, timeout=PATIENCE):
        """Reads until the response to `id`, keeping the updates on the way; returns the response."""
        deadline = time.monotonic() + timeout
        while True:
            message = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            assert message is not None, f"akern-acp exited: {self.proc.stderr.read()}"
            if message.get("method") == "session/update":
                self.updates.append(message["params"]["update"])
            elif message.get("id") == id:
                return message

    def call(self, method, params=None):
        response = self.wait(self.send(method, params))
        assert "error" not in response, response
        return response["result"]

    def quiet_for(self, seconds):
        """True when nothing arrives for `seconds`."""
        try:
            message = self.lines.get(timeout=seconds)
        except queue.Empty:
            return True
        self.lines.put(message)
        return False

    def take_updates(self):
        taken, self.updates = self.updates, []
        return taken

    def close(self):
        self.proc.stdin.close()
        assert self.proc.wait(PATIENCE) == 0


@pytest.fixture
def acp(akern_env, tmp_path):
    env = {**akern_env, "AKERN_MODEL": "mock/echo", "TMPDIR": str(tmp_path)}
    clients = []

    def start(*args):
        client = Client(env, tmp_path, *args)
        client.call("initialize", {"protocolVersion": 1, "clientCapabilities": {}})
        clients.append(client)
        return client

    yield start
    for client in clients:
        if client.proc.poll() is None:
            client.close()


def new_session(client, cwd):
    return client.call("session/new", {"cwd": str(cwd), "mcpServers": []})["sessionId"]


def prompt(client, session, text):
    return client.call("session/prompt", {"sessionId": session, "prompt": [{"type": "text", "text": text}]})


def texts(updates, kind="agent_message_chunk"):
    return "".join(u["content"]["text"] for u in updates if u["sessionUpdate"] == kind)


def session_lines(tmp_path, session):
    return [json.loads(line) for line in (tmp_path / "sessions" / f"{session}.jsonl").read_text().splitlines()]


def hellos(tmp_path, session):
    """The hello line of each akern started on the session: its model and params."""
    return [e for e in session_lines(tmp_path, session) if e["type"] == "hello"]


def answered_by(tmp_path, session):
    """The hello of the akern that wrote the session's last message."""
    lines = session_lines(tmp_path, session)
    last = max(i for i, e in enumerate(lines) if e["type"] == "message")
    return [e for e in lines[:last] if e["type"] == "hello"][-1]


def test_initialize_says_what_akern_acp_offers(akern_env, tmp_path):
    client = Client({**akern_env, "AKERN_MODEL": "mock/echo"}, tmp_path)
    result = client.call("initialize", {"protocolVersion": 1, "clientCapabilities": {}})
    assert result["protocolVersion"] == 1
    assert result["agentCapabilities"]["loadSession"] is True
    assert result["agentCapabilities"]["promptCapabilities"]["image"] is True
    assert result["authMethods"] == []
    client.close()


def test_a_prompt_streams_the_answer_and_ends_the_turn_with_its_usage(acp, tmp_path):
    client = acp()
    session = client.call("session/new", {"cwd": str(tmp_path), "mcpServers": []})
    assert session["models"]["currentModelId"] == "mock/echo"
    result = prompt(client, session["sessionId"], "hi")
    assert result["stopReason"] == "end_turn"
    assert (result["usage"]["inputTokens"], result["usage"]["outputTokens"]) == (1, 1)
    assert texts(client.take_updates()) == "echo: hi"
    assert (tmp_path / "sessions" / f"{session['sessionId']}.jsonl").exists()


def test_a_shell_call_is_a_tool_call_closed_with_its_output(acp, tmp_path):
    client = acp()
    session = new_session(client, tmp_path)
    prompt(client, session, "!echo hi")
    updates = client.take_updates()
    [start] = [u for u in updates if u["sessionUpdate"] == "tool_call"]
    assert (start["title"], start["kind"], start["status"], start["rawInput"]) == (
        "echo hi", "execute", "in_progress", {"command": "echo hi"})
    [end] = [u for u in updates if u["sessionUpdate"] == "tool_call_update" and u["status"] != "in_progress"]
    assert (end["toolCallId"], end["status"]) == (start["toolCallId"], "completed")
    assert end["content"] == [{"type": "content", "content": {"type": "text", "text": "```\n[exit 0]\nhi\n```"}}]
    assert texts(updates).startswith("The tool said: [exit 0]")


def test_an_edit_is_shown_as_a_diff_of_its_file(acp, tmp_path):
    (tmp_path / "notes.txt").write_text("alpha\n")
    client = acp()
    session = new_session(client, tmp_path)
    prompt(client, session, tool("edit", path="notes.txt", old="alpha", new="beta"))
    [start] = [u for u in client.take_updates() if u["sessionUpdate"] == "tool_call"]
    diff = {"type": "diff", "path": str(tmp_path / "notes.txt"), "oldText": "alpha", "newText": "beta"}
    assert (start["kind"], start["locations"], start["content"]) == ("edit", [{"path": diff["path"]}], [diff])
    assert (tmp_path / "notes.txt").read_text() == "beta\n"


def test_a_job_that_outlives_its_prompt_reports_at_the_next_prompt(acp, tmp_path):
    client = acp()
    session = new_session(client, tmp_path)
    ended = tmp_path / "ended"
    assert prompt(client, session, tool("shell", command=f"sleep 0.3 && echo bg && touch {ended}", wait=0))["stopReason"] == "end_turn"
    [start] = [u for u in client.take_updates() if u["sessionUpdate"] == "tool_call"]
    wait_for(ended.exists)
    assert client.quiet_for(0.5), "updates arrived while no prompt was open"
    prompt(client, session, "next")
    updates = client.take_updates()
    [end] = [u for u in updates if u["sessionUpdate"] == "tool_call_update" and u["status"] == "completed"]
    assert end["toolCallId"] == start["toolCallId"]
    assert end["rawOutput"]["status"] == "exit 0"
    said = texts(updates)
    assert said.index("The tool said: [exit 0]\nbg") < said.index("echo: next")


def test_with_background_send_a_jobs_result_comes_as_soon_as_it_ends(acp, tmp_path):
    client = acp("--background", "send")
    session = new_session(client, tmp_path)
    prompt(client, session, tool("shell", command="sleep 0.3 && echo bg", wait=0))
    client.take_updates()
    deadline = time.monotonic() + PATIENCE
    while "bg" not in texts(client.updates):
        client.updates.append(client.lines.get(timeout=deadline - time.monotonic())["params"]["update"])
    assert any(u["sessionUpdate"] == "tool_call_update" and u["status"] == "completed" for u in client.updates)


def test_until_settled_a_prompt_waits_for_its_jobs_and_the_runs_they_start(acp, tmp_path):
    client = acp("--until", "settled")
    session = new_session(client, tmp_path)
    result = prompt(client, session, tool("shell", command="sleep 0.3 && echo bg", wait=0))
    assert result["stopReason"] == "end_turn"
    assert result["usage"]["inputTokens"] == 3  # the call, the reply to its placeholder, the run its result started
    assert "The tool said: [exit 0]\nbg" in texts(client.take_updates())


def test_cancel_aborts_the_run_and_the_prompt_ends_cancelled(acp, tmp_path):
    client = acp()
    session = new_session(client, tmp_path)
    id = client.send("session/prompt", {"sessionId": session, "prompt": [{"type": "text", "text": "!sleep 30"}]})
    deadline = time.monotonic() + PATIENCE
    while not any(u["sessionUpdate"] == "tool_call" for u in client.updates):
        message = client.lines.get(timeout=deadline - time.monotonic())
        client.updates.append(message["params"]["update"])
    client.send("session/cancel", {"sessionId": session}, id=False)
    response = client.wait(id)
    assert response["result"]["stopReason"] == "cancelled"
    [end] = [u for u in client.take_updates() if u["sessionUpdate"] == "tool_call_update"]
    assert end["status"] == "failed"


def test_a_session_loads_in_a_new_process_and_replays_its_conversation(acp, tmp_path):
    first = acp()
    session = new_session(first, tmp_path)
    prompt(first, session, "!echo hi")
    first.close()
    second = acp()
    second.call("session/load", {"sessionId": session, "cwd": str(tmp_path), "mcpServers": []})
    replayed = second.take_updates()
    assert texts(replayed, "user_message_chunk") == "!echo hi"
    assert [u["sessionUpdate"] for u in replayed if u["sessionUpdate"].startswith("tool_call")] == ["tool_call", "tool_call_update"]
    assert texts(replayed).startswith("The tool said:")
    assert texts([]) == ""
    prompt(second, session, "again")
    assert texts(second.take_updates()) == "echo: again"


def test_loading_a_session_that_does_not_exist_fails(acp, tmp_path):
    client = acp()
    response = client.wait(client.send("session/load", {"sessionId": "nope", "cwd": str(tmp_path), "mcpServers": []}))
    assert response["error"]["code"] == -32002


def test_set_model_restarts_akern_on_the_new_model_with_the_same_session(acp, tmp_path):
    client = acp()
    session = new_session(client, tmp_path)
    prompt(client, session, "one")
    client.call("session/set_model", {"sessionId": session, "modelId": "mock/other"})
    prompt(client, session, "two")
    assert [h["model"] for h in hellos(tmp_path, session)] == ["mock/echo", "mock/other"]
    entries = session_lines(tmp_path, session)
    assert [e["meta"]["model"] for e in entries if e["type"] == "message" and e["message"]["role"] == "assistant"] == [
        "mock/echo", "mock/other"]
    assert not [e for e in entries if e["type"] == "settings"]


def test_a_session_that_cannot_start_fails_with_akerns_reason(akern_env, home, tmp_path):
    client = Client({**akern_env, **home.env}, tmp_path)
    client.call("initialize", {"protocolVersion": 1})
    response = client.wait(client.send("session/new", {"cwd": str(tmp_path), "mcpServers": []}))
    assert "no model" in response["error"]["message"]
    client.close()


def test_a_cancel_right_after_the_prompt_stops_its_run(acp, tmp_path):
    client = acp()
    session = new_session(client, tmp_path)
    started = time.monotonic()
    id = client.send("session/prompt", {"sessionId": session, "prompt": [{"type": "text", "text": "!sleep 5"}]})
    client.send("session/cancel", {"sessionId": session}, id=False)
    assert client.wait(id)["result"]["stopReason"] == "cancelled"
    assert time.monotonic() - started < 4


def test_a_client_that_goes_away_stops_its_jobs_so_the_session_can_be_loaded_again(acp, tmp_path):
    first = acp()
    session = new_session(first, tmp_path)
    prompt(first, session, tool("shell", command="sleep 20", wait=0))
    first.close()  # waits for akern-acp to exit
    second = acp()
    second.call("session/load", {"sessionId": session, "cwd": str(tmp_path), "mcpServers": []})
    assert texts(second.take_updates(), "user_message_chunk").startswith("!@shell")


def test_a_new_session_offers_its_providers_models_and_a_thinking_effort_choice(acp, tmp_path):
    client = acp()
    result = client.call("session/new", {"cwd": str(tmp_path), "mcpServers": []})
    assert result["models"] == {"currentModelId": "mock/echo",
                                "availableModels": [{"modelId": "mock/echo", "name": "mock/echo"}]}
    [effort] = result["configOptions"]
    assert (effort["id"], effort["category"], effort["type"], effort["currentValue"]) == (
        "effort", "thought_level", "select", "default")
    assert [o["value"] for o in effort["options"]] == ["default", "low", "medium", "high"]


def test_the_effort_option_sets_the_providers_effort_param(acp, tmp_path):
    client = acp()
    session = new_session(client, tmp_path)
    result = client.call("session/set_config_option", {"sessionId": session, "configId": "effort", "value": "high"})
    assert result["configOptions"][0]["currentValue"] == "high"
    prompt(client, session, "hi")
    assert answered_by(tmp_path, session)["params"] == {"effort": "high"}


def test_slash_commands_are_answered_by_akern_acp_and_never_reach_the_model(acp, tmp_path):
    client = acp()
    session = new_session(client, tmp_path)
    assert prompt(client, session, "/model")["stopReason"] == "end_turn"
    assert texts(client.take_updates()).startswith("Model: mock/echo\n\nmock offers:\n- mock/echo")
    prompt(client, session, "/model other")
    assert texts(client.take_updates()) == "Model: mock/other"
    prompt(client, session, "/effort max")
    updates = client.take_updates()
    assert texts(updates) == "Effort: max"
    [changed] = [u for u in updates if u["sessionUpdate"] == "config_option_update"]
    assert changed["configOptions"][0]["currentValue"] == "max"
    prompt(client, session, "hi")
    assert texts(client.take_updates()) == "echo: hi"
    hello = answered_by(tmp_path, session)
    assert (hello["model"], hello["params"]) == ("mock/other", {"effort": "max"})
    users = [b["text"] for e in session_lines(tmp_path, session)
             if e["type"] == "message" and e["message"]["role"] == "user" for b in e["message"]["content"]]
    assert users == ["hi"]


def test_a_loaded_session_reports_the_model_and_effort_it_was_left_with(acp, tmp_path):
    first = acp()
    session = new_session(first, tmp_path)
    prompt(first, session, "/model other")
    prompt(first, session, "/effort low")
    first.close()
    loaded = acp().call("session/load", {"sessionId": session, "cwd": str(tmp_path), "mcpServers": []})
    assert loaded["models"]["currentModelId"] == "mock/other"
    assert loaded["configOptions"][0]["currentValue"] == "low"


# ---- protocol 2 ----

import base64


class Client2(Client):
    """A protocol 2 client: updates are kept with the session they are for."""

    def wait(self, id, timeout=PATIENCE):
        deadline = time.monotonic() + timeout
        while True:
            message = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            assert message is not None, f"akern-acp exited: {self.proc.stderr.read()}"
            if message.get("method") == "session/update":
                self.updates.append((message["params"]["sessionId"], message["params"]["update"]))
            elif message.get("id") == id:
                return message

    def until(self, done, timeout=PATIENCE):
        """Reads updates until `done(updates)`; returns them."""
        deadline = time.monotonic() + timeout
        while not done(self.updates):
            message = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            assert message is not None, f"akern-acp exited: {self.proc.stderr.read()}"
            if message.get("method") == "session/update":
                self.updates.append((message["params"]["sessionId"], message["params"]["update"]))
        return self.updates


def of(updates, session, kind=None):
    return [u for s, u in updates if s == session and (kind is None or u["sessionUpdate"] == kind)]


def states(updates, session):
    return [u for u in of(updates, session, "state_update")]


def idle(session, n=1):
    """A condition for until(): the session has gone idle `n` times."""
    return lambda updates: sum(u["state"] == "idle" for u in states(updates, session)) >= n


def message_text(updates, session, kind="agent_message"):
    chunks = {}
    for u in of(updates, session):
        if u["sessionUpdate"] == kind:
            chunks[u["messageId"]] = "".join(b["text"] for b in u["content"] if b["type"] == "text")
        elif u["sessionUpdate"] == kind + "_chunk":
            chunks[u["messageId"]] = chunks.get(u["messageId"], "") + u["content"]["text"]
    return "".join(chunks.values())


def calls(updates, session):
    """Each tool call's updates merged, by id, in the order they were first seen."""
    merged = {}
    for u in of(updates, session, "tool_call_update"):
        merged.setdefault(u["toolCallId"], {}).update(u)
    return list(merged.values())


@pytest.fixture
def acp2(akern_env, tmp_path):
    env = {**akern_env, "AKERN_MODEL": "mock/echo", "TMPDIR": str(tmp_path)}
    clients = []

    def start(*args):
        client = Client2(env, tmp_path, *args)
        client.call("initialize", {"protocolVersion": 2, "info": {"name": "test", "version": "0"}, "capabilities": {}})
        clients.append(client)
        return client

    yield start
    for client in clients:
        if client.proc.poll() is None:
            client.close()


def say(client, session, text, **params):
    return client.call("session/prompt", {"sessionId": session, "prompt": [{"type": "text", "text": text}], **params})


def test_v2_initialize_negotiates_protocol_2(akern_env, tmp_path):
    client = Client2({**akern_env, "AKERN_MODEL": "mock/echo"}, tmp_path)
    result = client.call("initialize", {"protocolVersion": 2, "info": {"name": "t", "version": "0"}, "capabilities": {}})
    assert result["protocolVersion"] == 2
    assert result["info"]["name"] == "akern-acp"
    assert result["capabilities"]["session"]["prompt"] == {"image": {}, "embeddedContext": {}}
    client.close()


def test_v2_a_prompt_is_answered_with_its_message_id_and_the_run_is_running_then_idle(acp2, tmp_path):
    client = acp2()
    created = client.call("session/new", {"cwd": str(tmp_path)})
    session = created["sessionId"]
    assert [o["configId"] for o in created["configOptions"]] == ["model", "effort"]
    assert [c["name"] for c in created["availableCommands"]] == ["model", "effort"]
    message_id = say(client, session, "hi")["messageId"]
    updates = client.until(idle(session))
    [user] = of(updates, session, "user_message")
    assert (user["messageId"], user["content"]) == (message_id, [{"type": "text", "text": "hi"}])
    assert [s["state"] for s in states(updates, session)] == ["running", "idle"]
    end = states(updates, session)[-1]
    assert (end["stopReason"], end["background"]) == ("end_turn", [])
    assert end["_meta"]["io.github.wrongbad.akern/usage"]["input"] == 1
    assert message_text(updates, session) == "echo: hi"
    assert [u["sessionUpdate"] for u in of(updates, session, "session_info_update")] == ["session_info_update"]


def test_v2_a_shell_call_is_a_terminal_its_output_streams_to(acp2, tmp_path):
    client = acp2()
    session = client.call("session/new", {"cwd": str(tmp_path)})["sessionId"]
    say(client, session, "!echo hi")
    updates = client.until(idle(session))
    [call] = calls(updates, session)
    assert (call["name"], call["kind"], call["status"], call["title"]) == ("shell", "execute", "completed", "echo hi")
    assert call["content"] == [{"type": "terminal", "terminalId": call["toolCallId"]}]
    terminal = {}
    for u in of(updates, session, "terminal_update"):
        terminal.update(u)
    assert (terminal["command"], terminal["cwd"], terminal["exitStatus"]) == ("echo hi", str(tmp_path), {"exitCode": 0})
    output = b"".join(base64.b64decode(u["data"]) for u in of(updates, session, "terminal_output_chunk"))
    assert output == b"hi\n"


def test_v2_a_job_is_background_work_that_reports_and_wakes_the_model_while_idle(acp2, tmp_path):
    client = acp2()
    session = client.call("session/new", {"cwd": str(tmp_path)})["sessionId"]
    say(client, session, tool("shell", command="sleep 0.5 && echo bg", wait=0))
    updates = client.until(idle(session))
    [call] = calls(updates, session)
    assert states(updates, session)[-1]["background"] == [{"type": "tool_call", "toolCallId": call["toolCallId"]}]
    updates = client.until(idle(session, 2))  # the job ends, and its result starts a run of its own
    assert calls(updates, session)[0]["status"] == "completed"
    assert [s["state"] for s in states(updates, session)] == ["running", "idle", "running", "idle"]
    assert states(updates, session)[-1]["background"] == []
    assert "The tool said: [exit 0]\nbg" in message_text(updates, session)


def test_v2_a_prompt_sent_while_a_run_works_steers_it(acp2, tmp_path):
    client = acp2()
    session = client.call("session/new", {"cwd": str(tmp_path)})["sessionId"]
    client.send("session/prompt", {"sessionId": session, "prompt": [{"type": "text", "text": "!sleep 1"}]})
    client.until(lambda updates: calls(updates, session))
    second = say(client, session, "and then")["messageId"]
    updates = client.until(lambda updates: "echo: and then" in message_text(updates, session))
    assert second in [u["messageId"] for u in of(updates, session, "user_message")]


def test_v2_cancel_ends_the_run_cancelled(acp2, tmp_path):
    client = acp2()
    session = client.call("session/new", {"cwd": str(tmp_path)})["sessionId"]
    say(client, session, "!sleep 30")
    client.until(lambda updates: calls(updates, session))
    client.send("session/cancel", {"sessionId": session}, id=False)
    updates = client.until(idle(session))
    assert states(updates, session)[-1]["stopReason"] == "cancelled"
    assert calls(updates, session)[0]["status"] == "cancelled"


def test_v2_a_job_is_cancelled_by_its_tool_call_id(acp2, tmp_path):
    client = acp2()
    session = client.call("session/new", {"cwd": str(tmp_path)})["sessionId"]
    say(client, session, tool("shell", command="echo ready && sleep 30", wait=0))
    updates = client.until(idle(session))
    [call] = calls(updates, session)
    assert call["capabilities"] == {"cancel": {}}
    client.until(lambda updates: b"ready\n" in b"".join(base64.b64decode(u["data"]) for u in of(updates, session, "terminal_output_chunk")))
    client.call("tool_call/cancel", {"sessionId": session, "toolCallId": call["toolCallId"]})
    updates = client.until(lambda updates: calls(updates, session)[0]["status"] != "in_progress")
    assert calls(updates, session)[0]["status"] == "cancelled"
    exits = [u["exitStatus"] for u in of(updates, session, "terminal_update") if "exitStatus" in u]
    assert exits == [{"signal": "SIGTERM"}]


def test_v2_an_edit_is_a_change_with_a_git_patch(acp2, tmp_path):
    (tmp_path / "notes.txt").write_text("one\nalpha two\nthree\n")
    client = acp2()
    session = client.call("session/new", {"cwd": str(tmp_path)})["sessionId"]
    say(client, session, tool("edit", path="notes.txt", old="alpha", new="beta"))
    [call] = calls(client.until(idle(session)), session)
    [diff] = call["content"][:1]
    assert diff["changes"] == [{"operation": "modify", "path": str(tmp_path / "notes.txt"), "fileType": "text"}]
    assert diff["patch"] == {"format": "git_patch", "text": "diff --git a/notes.txt b/notes.txt\n--- a/notes.txt\n"
                             "+++ b/notes.txt\n@@ -2,1 +2,1 @@\n-alpha two\n+beta two\n"}


def test_v2_a_sub_agent_is_a_child_session_read_from_its_file(acp2, tmp_path):
    client = acp2()
    session = client.call("session/new", {"cwd": str(tmp_path)})["sessionId"]
    say(client, session, tool("shell", command="mkdir -p agents && akern -p -s agents/helper.jsonl hello", wait=0))
    child = f"{session}/agents/helper"
    updates = client.until(lambda updates: any(u.get("state", {}).get("state") == "idle"
                                               for u in of(updates, session, "subagent_update")))
    announced = of(updates, session, "subagent_update")
    assert (announced[0]["sessionId"], announced[0]["title"], announced[0]["state"]) == (
        child, "agents/helper", {"state": "running"})
    assert announced[-1]["state"] == {"state": "idle", "stopReason": "end_turn"}
    [prompt] = of(updates, child, "session_message")
    assert (prompt["senderSessionId"], prompt["content"]) == (session, [{"type": "text", "text": "hello"}])
    assert message_text(updates, child) == "echo: hello"
    [call] = calls(updates, session)
    assert {"type": "session", "sessionId": child} in call["content"]


def test_v2_resume_replays_only_when_asked(acp2, tmp_path):
    first = acp2()
    session = first.call("session/new", {"cwd": str(tmp_path)})["sessionId"]
    say(first, session, "!echo hi")
    first.until(idle(session))
    first.close()
    second = acp2()
    second.call("session/resume", {"sessionId": session, "cwd": str(tmp_path)})
    updates = second.take_updates()
    assert [u["sessionUpdate"] for u in of(updates, session)] == ["state_update", "session_info_update"]
    second.call("session/close", {"sessionId": session})
    second.take_updates()
    second.call("session/resume", {"sessionId": session, "cwd": str(tmp_path), "replayFrom": {"type": "start"}})
    updates = second.take_updates()
    kinds = [u["sessionUpdate"] for u in of(updates, session)]
    assert kinds.index("replay_complete") < kinds.index("state_update")
    assert message_text(updates, session, "user_message") == "!echo hi"
    assert message_text(updates, session).startswith("The tool said:")
    assert [c["status"] for c in calls(updates, session)] == ["completed"]
    say(second, session, "again")
    assert "echo: again" in message_text(second.until(idle(session)), session)


def test_v2_sessions_are_listed_with_their_cwd_and_title(acp2, tmp_path):
    client = acp2()
    session = client.call("session/new", {"cwd": str(tmp_path)})["sessionId"]
    say(client, session, "find the bug\nin parse()")
    client.until(idle(session))
    [listed] = client.call("session/list", {"cwd": str(tmp_path)})["sessions"]
    assert (listed["sessionId"], listed["cwd"], listed["title"]) == (session, str(tmp_path), "find the bug…")
    assert client.call("session/list", {"cwd": "/elsewhere"})["sessions"] == []


def test_v2_close_ends_akern_and_the_session(acp2, tmp_path):
    client = acp2()
    session = client.call("session/new", {"cwd": str(tmp_path)})["sessionId"]
    say(client, session, tool("shell", command="sleep 30", wait=0))
    client.until(idle(session))
    assert client.call("session/close", {"sessionId": session}) == {}
    response = client.wait(client.send("session/prompt", {"sessionId": session, "prompt": [{"type": "text", "text": "x"}]}))
    assert response["error"]["code"] == -32002
    assert client.call("session/close", {"sessionId": session}) == {}


def test_v2_commands_and_config_options_change_the_model_and_effort(acp2, tmp_path):
    client = acp2()
    session = client.call("session/new", {"cwd": str(tmp_path)})["sessionId"]
    say(client, session, "/effort high")
    updates = client.until(idle(session))
    assert message_text(updates, session) == "Effort: high"
    [changed] = of(updates, session, "config_option_update")
    assert changed["configOptions"][1]["currentValue"] == "high"
    result = client.call("session/set_config_option", {"sessionId": session, "configId": "model", "type": "id", "value": "mock/other"})
    assert result["configOptions"][0]["currentValue"] == "mock/other"
    say(client, session, "hi")
    client.until(idle(session, 2))
    hello = answered_by(tmp_path, session)
    assert (hello["model"], hello["params"]) == ("mock/other", {"effort": "high"})


def test_v2_a_sub_agents_own_sub_agent_is_announced_on_its_session(acp2, tmp_path):
    client = acp2()
    session = client.call("session/new", {"cwd": str(tmp_path)})["sessionId"]
    inner = "!akern -p -s agents/b.jsonl deep"
    say(client, session, tool("shell", command=f"mkdir -p agents && akern -p -s agents/a.jsonl '{inner}'", wait=0))
    a, b = f"{session}/agents/a", f"{session}/agents/b"
    updates = client.until(lambda updates: any(u.get("state", {}).get("state") == "idle"
                                               for u in of(updates, session, "subagent_update")))
    assert [u["sessionId"] for u in of(updates, a, "subagent_update")][:1] == [b]
    assert of(updates, a, "subagent_update")[-1]["state"] == {"state": "idle", "stopReason": "end_turn"}
    assert message_text(updates, b) == "echo: deep"
    [call] = calls(updates, a)
    assert {"type": "session", "sessionId": b} in call["content"]


def ids_of(updates, session, kind):
    """Each entity's id of one kind, in the order first seen."""
    return list(dict.fromkeys(u["messageId"] for u in of(updates, session) if u["sessionUpdate"] in (kind, kind + "_chunk")))


def replay(acp2, session, cwd):
    client = acp2()
    client.call("session/resume", {"sessionId": session, "cwd": str(cwd), "replayFrom": {"type": "start"}})
    return client.take_updates()


def test_v2_messages_keep_their_ids_from_live_to_replay(acp2, tmp_path):
    client = acp2()
    session = client.call("session/new", {"cwd": str(tmp_path)})["sessionId"]
    prompt_id = say(client, session, "hi")["messageId"]
    live = client.until(idle(session))
    client.close()
    replayed = replay(acp2, session, tmp_path)
    assert ids_of(live, session, "user_message") == ids_of(replayed, session, "user_message") == [prompt_id]
    [answer_id] = ids_of(live, session, "agent_message")
    assert ids_of(replayed, session, "agent_message") == [answer_id]
    assert not answer_id.startswith(prompt_id)


def test_v2_prompts_delivered_together_replay_as_their_own_messages(acp2, tmp_path):
    client = acp2()
    session = client.call("session/new", {"cwd": str(tmp_path)})["sessionId"]
    client.send("session/prompt", {"sessionId": session, "prompt": [{"type": "text", "text": "!sleep 0.5"}]})
    client.until(lambda updates: calls(updates, session))
    later = {"_meta": {"io.github.wrongbad.akern/when": "idle"}}
    a = client.send("session/prompt", {"sessionId": session, "prompt": [{"type": "text", "text": "one"}], **later})
    b = client.send("session/prompt", {"sessionId": session, "prompt": [{"type": "text", "text": "two"}], **later})
    first, second = (client.wait(r)["result"]["messageId"] for r in (a, b))
    client.until(idle(session))  # idle items go in when the run's last turn ends, in the same run
    client.close()
    entries = [json.loads(line) for line in (tmp_path / "sessions" / f"{session}.jsonl").read_text().splitlines()]
    [together] = [e for e in entries if len(e.get("meta", {}).get("items", [])) == 2]
    assert [i["id"] for i in together["meta"]["items"]] == [first, second]
    replayed = replay(acp2, session, tmp_path)
    messages = {u["messageId"]: u["content"] for u in of(replayed, session, "user_message")}
    assert messages[first] == [{"type": "text", "text": "one"}]
    assert messages[second] == [{"type": "text", "text": "two"}]


def test_v2_a_cancel_right_after_a_prompt_leaves_no_prompt_hanging(acp2, tmp_path):
    client = acp2()
    session = client.call("session/new", {"cwd": str(tmp_path)})["sessionId"]
    request = client.send("session/prompt", {"sessionId": session, "prompt": [{"type": "text", "text": "!sleep 30"}]})
    client.send("session/cancel", {"sessionId": session}, id=False)
    response = client.wait(request)
    if "error" in response:  # handed back before akern took it: never inserted
        assert response["error"]["code"] == -32800
        assert client.quiet_for(0.5) and not states(client.updates, session)
    else:  # akern had started on it: the run ends cancelled
        assert states(client.until(idle(session)), session)[-1]["stopReason"] == "cancelled"
