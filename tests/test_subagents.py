import json
import re

from conftest import Run, parse_shell, seen_pending, shell, tool, wait_for


def test_a_sub_agent_job_answers_on_stdout_in_the_session_its_parent_names(kernel, tmp_path):
    run = kernel(tool("shell", command="akern -p -s agents/hi.jsonl 'say hi'", wait=0))
    assert run.pending
    assert (tmp_path / "agents" / "hi.jsonl").is_file()
    [(_, result)] = run.job_reports
    assert result["output"] == "echo: say hi\n"


def test_a_line_written_to_a_sub_agents_stdin_is_answered_while_its_call_runs(kernel, tmp_path):
    gate = tmp_path / "gate"
    sub_agent = Run(tmp_path / "agents" / "steer.jsonl", printing=True)
    run = kernel(tool("shell", command=f"akern -p -s agents/steer.jsonl '!gate {gate}'",
                      wait=0, eof=False),
                 later=[(lambda run: seen_pending()(run) and sub_agent.entries_of("message")[1:],
                         lambda run: tool("job", id=run.pending[0]["call"], stdin="also say hi\n"))],
                 during=lambda run: wait_for(lambda: "echo: also say hi" in sub_agent.replies)
                 and gate.write_text("released\n"))
    [wrote] = run.result_texts
    assert wrote == f"{run.pending[0]['call']}: wrote 12 bytes to its stdin."
    [(_, result)] = run.job_reports
    assert result["output"] == f"echo: also say hi\nThe tool said: [exit 0]\nwaiting on {gate}\nreleased\n\n"


def test_recursion_limit_counts_down_per_nested_kernel(kernel):
    run = kernel(None, "-p", tool("shell", command="akern -p '!akern -p hi'"),
                 env={"AKERN_RE_LIMIT": "2"})
    # The middle kernel's answer quotes the innermost result, header and all.
    middle = parse_shell(run.shell_result["output"].removeprefix("The tool said: "))
    assert middle["exit"] != 0
    assert "recursion limit reached" in middle["output"]
    assert "recursion limit reached" in run.output


def test_children_get_only_the_model_and_the_recursion_limit_from_the_kernel(kernel, home):
    home.mock("env | grep ^AKERN_ | sort >&2")
    run = kernel(shell("env | grep ^AKERN_ | sort"), env=home.env)
    inherited = f"AKERN_HOME={home.dir}\nAKERN_MODEL=mock/echo\nAKERN_RE_LIMIT=3"
    assert {n["message"] for n in run.events_of("notice")} == set(inherited.splitlines())
    assert run.shell_result["output"] == inherited + "\n"
