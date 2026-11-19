import os
import signal
import subprocess

import pytest

from conftest import BUILD, parallel, seen, shell, signal_when, tool, wait_for
from test_files import TINY_PNG


@pytest.mark.parametrize("sig", [signal.SIGTERM, signal.SIGHUP], ids=["term", "hup"])
def test_shutdown_mid_tool_answers_every_call_and_closes_every_job_then_exits_130(kernel, sig):
    run = kernel(parallel(tool("exec", argv=["sleeper", "30"], wait=False), shell("sleeper 30")),
                 during=signal_when((seen("tool_progress", 2), sig)), code=130)
    assert run.result_texts == ["Killed: the run was aborted."]
    [pending] = run.pending
    [(late, _)] = run.job_reports
    assert (late["tool_use_id"], late["is_error"]) == (pending["tool_use_id"], True)
    assert [e["reason"] for e in run.events_of("run_end")] == ["aborted"]


def test_shutdown_closes_a_job_with_its_usual_result_not_its_raw_stdout(kernel, tmp_path):
    png = tmp_path / "tiny.png"
    png.write_bytes(TINY_PNG)
    program = f"trap 'cat {png} && exit 0' TERM\necho ready >&2\nsleep 30 & wait"
    run = kernel(tool("exec", argv=["sh", "-c", program], stdout_type="image/png", wait=False),
                 during=signal_when((seen("tool_progress"), signal.SIGTERM)), code=130)
    [(_, result)] = run.job_reports
    assert result == {"stdout": f"({len(TINY_PNG)} bytes not shown: the program failed)", "stderr": "ready\n", "exit": 0}


def test_sigint_aborts_keeping_jobs_and_a_second_within_1s_shuts_down(kernel):
    run = kernel(parallel(tool("exec", argv=["sleeper", "30"], wait=False), shell("sleeper 30")),
                 during=signal_when((seen("tool_progress", 2), signal.SIGINT), (seen("run_end"), signal.SIGINT)), code=130)
    assert run.result_texts == ["Killed: the run was aborted."]
    assert len(run.pending) == len(run.job_reports) == 1
    assert [e["reason"] for e in run.events_of("run_end")] == ["aborted"]


def test_print_mode_writes_only_the_final_answer(kernel):
    run = kernel(shell("echo hi"), "-p")
    assert run.output == 'The tool said: {"stdout":"hi\\n","stderr":"","exit":0}\n'


def test_a_run_ending_in_error_exits_1_and_print_mode_says_why_on_stderr(kernel):
    run = kernel("hello", "-p", "-m", "nope/x", code=1)
    assert run.output == ""
    assert run.stderr == f"akern-loop: no provider for nope/x in {BUILD}/providers\n"


@pytest.mark.parametrize("args", [["-o", "max_procs=4"], ["--max-procs", "4"], ["--kill-grace", "soon"], ["-P", "effort"]],
                         ids=["removed-o", "unknown-flag", "bad-number", "param-without-value"])
def test_an_unknown_flag_or_a_bad_value_prints_usage_and_exits_2(kernel, args):
    run = kernel("hello", *args, code=2)
    assert run.output == ""
    assert "usage: akern-loop" in run.stderr


@pytest.mark.parametrize("limit", ["0", "-1", "deep"])
def test_a_recursion_limit_below_1_refuses_to_start(kernel, limit):
    run = kernel("hello", env={"AKERN_RECURSION_LIMIT": limit}, code=2)
    assert run.output == ""
    assert run.stderr.startswith("akern-loop: recursion limit reached")


def test_a_session_owned_by_a_running_kernel_cannot_be_opened(kernel, akern_env):
    second = {}

    def open_it_again(run):
        wait_for(lambda: run.events_of("tool_start"))
        second["run"] = subprocess.run(["akern-loop", "-m", "mock/echo", "-s", str(run.session)], env=akern_env,
                                       stdin=subprocess.DEVNULL, capture_output=True, text=True)
        os.kill(run.pid, signal.SIGINT)

    run = kernel(shell("sleep 30"), during=open_it_again)
    assert second["run"].returncode == 2
    assert second["run"].stderr == f"akern-loop: session {run.session} is owned by another kernel (pid {run.pid})\n"
