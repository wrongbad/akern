"""akern-agent: an ACP protocol 2 sub-agent run like `akern -p`. Against akern-acp end to end,
and against tests/bin/acp-fake for what akern-acp never does (permissions, failures)."""

import fcntl
import json
import signal
import subprocess
import time

import pytest

from conftest import PATIENCE, tool, wait_for


@pytest.fixture
def agent(akern_env, tmp_path):
    env = {**akern_env, "AKERN_MODEL": "mock/echo", "TMPDIR": str(tmp_path),
           "AKERN_ACP_SESSIONS": str(tmp_path / "acp-sessions"), "FAKE_ACP_LOG": str(tmp_path / "fake.log")}
    (tmp_path / "agents").mkdir()

    def run(*args, stdin="", code=0, extra_env=None):
        done = subprocess.run(["akern-agent", *args], cwd=tmp_path, env={**env, **(extra_env or {})}, input=stdin,
                              capture_output=True, text=True, timeout=PATIENCE)
        assert done.returncode == code, f"exit {done.returncode}: {done.stderr}"
        return done

    run.env = env
    return run


def entries(path):
    return [json.loads(line) for line in path.read_text().splitlines()]


def fake_log(tmp_path):
    return [json.loads(line) for line in (tmp_path / "fake.log").read_text().splitlines()]


# ---- against akern-acp ----


def test_the_answer_is_on_stdout_and_the_session_file_names_the_acp_session(agent, tmp_path):
    done = agent("-a", "akern-acp", "-s", "agents/hi.jsonl", "say hi")
    assert done.stdout == "echo: say hi\n"
    header, *lines = entries(tmp_path / "agents" / "hi.jsonl")
    assert (header["type"], header["agent"], header["cwd"]) == ("acp_session", "akern-acp", str(tmp_path))
    assert lines[0]["out"]["method"] == "initialize"
    assert any(line.get("in", {}).get("method") == "session/update" for line in lines)


def test_the_same_session_file_continues_the_same_acp_session(agent, tmp_path):
    agent("-a", "akern-acp", "-s", "agents/two.jsonl", "first")
    done = agent("-s", "agents/two.jsonl", "second")  # the agent command comes from the file
    assert done.stdout == "echo: second\n"
    session = entries(tmp_path / "agents" / "two.jsonl")[0]["sessionId"]
    akern_session = tmp_path / "acp-sessions" / f"{session}.jsonl"
    prompts = [e["message"]["content"] for e in entries(akern_session)
               if e["type"] == "message" and e["message"]["role"] == "user"]
    assert [p if isinstance(p, str) else p[0]["text"] for p in prompts] == ["first", "second"]


def test_it_waits_for_background_work_and_the_run_its_result_starts(agent):
    done = agent("-a", "akern-acp", tool("shell", command="sleep 0.5 && echo from-the-job", wait=0))
    assert done.stdout.endswith("from-the-job\n")


def test_a_line_on_stdin_steers_the_agent_while_it_works(agent, tmp_path):
    gate = tmp_path / "gate"
    proc = subprocess.Popen(["akern-agent", "-a", "akern-acp", "-s", "agents/steer.jsonl", f"!gate {gate}"], cwd=tmp_path,
                            env=agent.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True)
    log = tmp_path / "agents" / "steer.jsonl"
    wait_for(lambda: log.exists() and "tool_call_update" in log.read_text())
    proc.stdin.write("and then\n")
    proc.stdin.flush()
    wait_for(lambda: "echo: and then" in log.read_text())  # answered while the call still runs
    gate.write_text("released\n")
    out, err = proc.communicate(timeout=PATIENCE)
    assert proc.returncode == 0, err
    assert "echo: and then" in out


def test_an_abort_line_cancels_the_work_and_it_exits(agent, tmp_path):
    proc = subprocess.Popen(["akern-agent", "-a", "akern-acp", "-s", "agents/abort.jsonl", "!sleep 30"], cwd=tmp_path,
                            env=agent.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True)
    log = tmp_path / "agents" / "abort.jsonl"
    wait_for(lambda: log.exists() and "tool_call_update" in log.read_text())
    started = time.monotonic()
    proc.stdin.write('{"type":"abort"}\n')
    proc.stdin.flush()
    _, err = proc.communicate(timeout=PATIENCE)
    assert proc.returncode == 0, err
    assert time.monotonic() - started < 10


def test_sigterm_stops_the_agent_and_exits_130(agent, tmp_path):
    proc = subprocess.Popen(["akern-agent", "-a", "akern-acp", "-s", "agents/term.jsonl", "!sleep 30"], cwd=tmp_path,
                            env=agent.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    log = tmp_path / "agents" / "term.jsonl"
    wait_for(lambda: log.exists() and "tool_call_update" in log.read_text())
    proc.send_signal(signal.SIGTERM)
    assert proc.wait(PATIENCE) == 130


def test_an_akern_parent_delegates_to_an_acp_sub_agent_through_shell(kernel, tmp_path):
    run = kernel(tool("shell", command="akern-agent -a akern-acp -s agents/acp.jsonl 'hello from the parent'", wait=0))
    [(_, result)] = run.job_reports
    assert result["output"] == "echo: hello from the parent\n"
    assert entries(tmp_path / "agents" / "acp.jsonl")[0]["agent"] == "akern-acp"


def test_an_acp_sub_agent_can_delegate_to_one_of_its_own(kernel, tmp_path):
    inner = "akern-agent -a akern-acp -s agents/inner.jsonl deep"
    run = kernel(tool("shell", command=f"akern-agent -a akern-acp -s agents/middle.jsonl '!{inner}'", wait=0))
    [(_, result)] = run.job_reports
    assert result["output"] == "The tool said: [exit 0]\necho: deep\n"
    assert entries(tmp_path / "agents" / "inner.jsonl")[0]["cwd"] == str(tmp_path)


# ---- against acp-fake ----


def test_a_permission_request_is_refused_by_default_and_the_work_cancelled(agent, tmp_path):
    done = agent("-a", "acp-fake", "-s", "agents/ask.jsonl", "ask", code=3)
    assert done.stdout == "refused\n"
    assert "refused permission for Run this?: rm -rf build" in done.stderr
    assert "--allow -s agents/ask.jsonl" in done.stderr
    sent = fake_log(tmp_path)
    [answer] = [m for m in sent if "result" in m]
    assert answer["result"]["outcome"] == {"outcome": "selected", "optionId": "no"}
    assert sent[-1]["method"] == "session/cancel"


def test_allow_grants_permission_requests(agent):
    done = agent("-a", "acp-fake", "--allow", "ask")
    assert done.stdout == "allowed\n"


def test_work_that_ends_in_an_error_exits_1_with_its_message(agent):
    done = agent("-a", "acp-fake", "fail", code=1)
    assert done.stderr == "akern-agent: it broke\n"


def test_an_agent_that_dies_exits_1_with_the_end_of_its_stderr(agent, tmp_path):
    done = agent("-a", "acp-fake", "-s", "agents/die.jsonl", "die", code=1)
    assert "the agent exited before it was done (exit 3)" in done.stderr
    assert "fake: dying" in done.stderr
    assert (tmp_path / "agents" / "die.jsonl.stderr").read_text() == "fake: dying\n"


def test_lines_that_are_not_json_are_kept_and_skipped(agent, tmp_path):
    done = agent("-a", "acp-fake", "-s", "agents/noisy.jsonl", "noisy")
    assert done.stdout == "fake[small] turn 1: noisy\n"
    assert {"in_text": "starting up..."} in entries(tmp_path / "agents" / "noisy.jsonl")


def test_it_waits_until_the_background_list_is_empty(agent, tmp_path):
    done = agent("-a", "acp-fake", "-s", "agents/bg.jsonl", "background")
    assert done.stdout == "started it\n"
    updates = [e["in"]["params"]["update"] for e in entries(tmp_path / "agents" / "bg.jsonl")
               if e.get("in", {}).get("method") == "session/update"]
    assert updates[-1] == {"sessionUpdate": "state_update", "state": "idle", "stopReason": "end_turn",
                           "background": []}


def test_model_and_options_are_set_before_the_prompt(agent):
    done = agent("-a", "acp-fake", "-m", "big", "-o", "fast=true", "hi")  # matches the choice's name
    assert done.stdout == "fake[big-1] turn 1: hi\n"


def test_an_unknown_option_or_choice_lists_what_the_agent_has(agent):
    done = agent("-a", "acp-fake", "-o", "speed=3", "hi", code=2)
    assert "has no option speed; it has: model, fast" in done.stderr
    done = agent("-a", "acp-fake", "-m", "huge", "hi", code=2)
    assert "the choices are: small (Small), big-1 (Big)" in done.stderr


def test_a_session_the_agent_no_longer_has_cannot_be_continued(agent, tmp_path):
    agent("-a", "acp-fake", "-s", "agents/gone.jsonl", "hi")
    for f in (tmp_path / ".fake-acp").iterdir():
        f.unlink()
    done = agent("-s", "agents/gone.jsonl", "again", code=2)
    assert "session/resume failed: no such session" in done.stderr


def test_an_agent_that_is_not_protocol_2_is_refused(agent):
    done = agent("-a", "acp-fake", "hi", code=2, extra_env={"FAKE_ACP_VERSION": "1"})
    assert "speaks ACP protocol 1; akern-agent needs protocol 2" in done.stderr


def test_a_session_file_in_use_is_refused(agent, tmp_path):
    path = tmp_path / "agents" / "busy.jsonl"
    with open(path, "w") as held:
        fcntl.lockf(held, fcntl.LOCK_EX)
        done = agent("-a", "acp-fake", "-s", "agents/busy.jsonl", "hi", code=2)
    assert "in use by another akern-agent" in done.stderr


def test_without_an_agent_or_a_session_naming_one_it_is_a_usage_error(agent):
    done = agent("hi", code=2)
    assert "no agent: pass -a COMMAND" in done.stderr
