import json

from conftest import parallel, shell, tool


def test_a_prompt_gets_a_streamed_answer(kernel):
    run = kernel("hello")
    assert run.streamed == "echo: hello"
    assert run.replies == ["echo: hello"]


def test_an_unknown_tool_gets_an_error_result_the_model_can_read(kernel):
    run = kernel(tool("bash", command="ls"))
    assert "Unknown tool: bash. The tools are exec, read and write." in run.result_text


def test_a_call_whose_input_is_not_an_object_gets_an_error_result_and_the_turn_goes_on(kernel, home):
    raw = '{"argv": sh, "argv": sh}'
    run = kernel(parallel("@exec " + json.dumps(raw), shell("echo still ran")), env=home.env)
    error, ran = run.tool_results
    assert (error["is_error"], error["content"][0]["text"]) == (True, "input is not a JSON object: " + raw)
    assert json.loads(ran["content"][0]["text"])["stdout"] == "still ran\n"
    assert home.requests[1]["messages"][-1]["content"][0] == error
    assert run.replies[-1] == "The tool said: input is not a JSON object: " + raw


def test_parallel_exec_calls_answer_in_one_message(kernel):
    run = kernel(shell("echo one", "echo two"))
    assert len(run.calls) == 2
    results = [m for m in run.messages if any(b["type"] == "tool_result" for b in m["content"])]
    assert len(results) == 1
    assert [json.loads(text) for text in run.result_texts] == [
        {"stdout": "one\n", "stderr": "", "exit": 0},
        {"stdout": "two\n", "stderr": "", "exit": 0},
    ]
    assert run.replies[-1] == 'The tool said: {"stdout":"one\\n","stderr":"","exit":0}'


def test_exec_runs_argv_without_shell_splitting_or_expansion(kernel):
    run = kernel(tool("exec", argv=["printf", "[%s]", "a b", "$HOME"]))
    assert run.result_json["stdout"] == "[a b][$HOME]"


def test_exec_is_waited_for_when_nothing_else_arrives(kernel):
    run = kernel(tool("exec", argv=["sh", "-c", "sleep 0.3 && echo finished"]))
    assert not run.pending
    assert run.result_json["stdout"] == "finished\n"


def test_result_keeps_stdout_stderr_and_exit_apart_and_nonzero_exit_is_an_error(kernel):
    run = kernel(shell("echo out && echo err 1>&2 && exit 3"))
    assert run.result_json == {"stdout": "out\n", "stderr": "err\n", "exit": 3}
    assert run.tool_results[0]["is_error"] is True
