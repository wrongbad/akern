import json

from conftest import parallel, shell, tool


def test_a_prompt_gets_a_streamed_answer(kernel):
    run = kernel("hello")
    assert run.streamed == "echo: hello"
    assert run.replies == ["echo: hello"]


def test_an_unknown_tool_gets_an_error_result_the_model_can_read(kernel):
    run = kernel(tool("bash", command="ls"))
    assert "Unknown tool: bash. The tools are shell, job and edit." in run.result_text


def test_a_call_whose_input_is_not_an_object_gets_an_error_result_and_the_turn_goes_on(kernel, home):
    raw = '{"argv": sh, "argv": sh}'
    run = kernel(parallel("@shell " + json.dumps(raw), shell("echo still ran")), env=home.env)
    error, ran = run.tool_results
    assert (error["is_error"], error["content"][0]["text"]) == (True, "input is not a JSON object: " + raw)
    assert ran["content"][0]["text"] == "[exit 0]\nstill ran\n"
    assert home.requests[1]["messages"][-1]["content"][0] == error
    assert run.replies[-1] == "The tool said: input is not a JSON object: " + raw


def test_parallel_shell_calls_answer_in_one_message(kernel):
    run = kernel(shell("echo one", "echo two"))
    assert len(run.calls) == 2
    results = [m for m in run.messages if any(b["type"] == "tool_result" for b in m["content"])]
    assert len(results) == 1
    assert run.result_texts == ["[exit 0]\none\n", "[exit 0]\ntwo\n"]
    assert run.replies[-1] == "The tool said: [exit 0]\none\n"


def test_shell_runs_the_command_with_bash(kernel):
    run = kernel(tool("shell", command="[[ -n $BASH_VERSION ]] && printf '[%s]' 'a b' '$HOME'"))
    assert run.shell_result["output"] == "[a b][$HOME]"


def test_shell_is_waited_for_when_nothing_else_arrives(kernel):
    run = kernel(tool("shell", command="sleep 0.3 && echo finished"))
    assert not run.pending
    assert run.shell_result["output"] == "finished\n"


def test_result_keeps_stdout_stderr_and_exit_apart_and_nonzero_exit_is_an_error(kernel):
    run = kernel(shell("echo out && echo err 1>&2 && exit 3"))
    assert run.result_text == "[exit 3]\nout\nerr\n"
    assert run.tool_results[0]["is_error"] is True
