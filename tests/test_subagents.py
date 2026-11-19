import json

from conftest import Run, seen_pending, shell, tool, wait_for


def test_a_sub_agent_job_answers_on_stdout_in_the_session_its_parent_names(kernel, tmp_path):
    run = kernel(tool("exec", argv=["akern", "-p", "-s", "agents/hi.jsonl", "say hi"], wait=False))
    assert run.pending
    assert (tmp_path / "agents" / "hi.jsonl").is_file()
    [(_, result)] = run.job_reports
    assert result["stdout"] == "echo: say hi\n"


def test_a_line_written_to_a_sub_agents_stdin_is_answered_while_its_call_runs(kernel, tmp_path):
    gate = tmp_path / "gate"
    sub_agent = Run(tmp_path / "agents" / "steer.jsonl", printing=True)
    run = kernel(tool("exec", argv=["akern", "-p", "-s", "agents/steer.jsonl", f"!gate {gate}"],
                      wait=False, stdin_open=True),
                 later=[(lambda run: seen_pending()(run) and sub_agent.entries_of("message")[1:],
                         lambda run: tool("write", job=run.pending[0]["tool_use_id"], content="also say hi\n"))],
                 during=lambda run: wait_for(lambda: "echo: also say hi" in sub_agent.replies)
                 and gate.write_text("released\n"))
    [wrote] = run.result_texts
    assert wrote == f"Wrote 12 bytes to the stdin of {run.pending[0]['tool_use_id']}."
    [(_, result)] = run.job_reports
    answer_to_the_line, answer_to_the_call = result["stdout"].splitlines()
    assert answer_to_the_line == "echo: also say hi"
    assert answer_to_the_call.startswith('The tool said: {"stdout":"released\\n"')


def test_recursion_limit_counts_down_per_nested_kernel(kernel):
    run = kernel(None, "-p", tool("exec", argv=["akern", "-p", "!akern -p hi"]),
                 env={"AKERN_RECURSION_LIMIT": "2"})
    middle = json.loads(run.result_json["stdout"].removeprefix("The tool said: "))
    assert middle["exit"] != 0
    assert "recursion limit reached" in middle["stderr"]
    assert "recursion limit reached" in run.output


def test_children_get_only_the_model_and_the_recursion_limit_from_the_kernel(kernel, home):
    home.mock("env | grep ^AKERN_ | sort >&2")
    run = kernel(shell("env | grep ^AKERN_ | sort"), env=home.env)
    inherited = f"AKERN_HOME={home.dir}\nAKERN_MODEL=mock/echo\nAKERN_RECURSION_LIMIT=3"
    assert {n["message"] for n in run.events_of("notice")} == set(inherited.splitlines())
    assert run.result_json["stdout"] == inherited + "\n"
