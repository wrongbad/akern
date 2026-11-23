import json

from conftest import PATIENCE, executable, seen


def answer(text):
    return {"type": "result", "stop": "end_turn", "message": {"role": "assistant", "content": [{"type": "text", "text": text}]}}


def test_a_result_after_a_provider_error_line_wins_and_the_run_ends_done(kernel, script):
    run = kernel("hi", env=script({"type": "error", "kind": "overloaded", "message": "retrying"}, answer("recovered")))
    [end] = run.events_of("run_end")
    assert (end["reason"], end["error"]) == ("done", None)
    assert run.replies == ["recovered"]


def test_a_provider_line_longer_than_64_mib_is_dropped_whole_and_the_provider_stopped(kernel, script):
    long_line = {"type": "tool_use_delta", "id": "tu_1", "json": "x" * (64 << 20)}  # not echoed when aborted
    run = kernel("hi", env=script(long_line, {"x-sleep": PATIENCE}, answer("never sent")))
    assert not run.events_of("tool_use_delta")
    assert not run.events_of("error")
    assert run.replies == []
    assert [e["reason"] for e in run.events_of("run_end")] == ["done"]


def test_a_param_is_sent_as_json_when_it_parses_and_as_a_string_otherwise(kernel, home):
    kernel("hi", "-P", "max_tokens=64", "--param", 'thinking={"type":"adaptive"}', "-P", "effort=high", env=home.env)
    assert home.requests[0]["params"] == {"max_tokens": 64, "thinking": {"type": "adaptive"}, "effort": "high"}


def test_provider_stderr_lines_are_notices_from_the_provider(kernel, home):
    home.mock('echo "warming up" >&2')
    run = kernel("hi", env=home.env)
    assert run.events_of("notice") == [{"type": "notice", "source": "provider", "message": "warming up"}]


def test_a_provider_in_akern_home_shadows_the_one_of_the_same_name_in_system(kernel, home):
    kernel("hi", env=home.env)
    (home.dir / "providers" / "mock").unlink()
    run = kernel("hi", env=home.env)
    assert len(home.requests) == 1
    assert run.events_of("run_end")[0]["reason"] == "done"


def test_the_system_prompt_is_the_output_of_the_first_system_prompt_program_and_null_when_empty_or_missing(kernel, home):
    program = executable(home.dir / "system-prompt", "#!/bin/sh\necho from home\n")
    kernel("hi", env=home.env)
    executable(program, "#!/bin/sh\n")
    kernel("hi", env=home.env)
    program.unlink()
    (home.dir / "system" / "system-prompt").unlink()
    kernel("hi", env=home.env)
    assert [r["system"] for r in home.requests] == ["from home\n", None, None]


def test_s_reads_its_file_once_and_skips_the_system_prompt_program(kernel, home, tmp_path):
    program = home.dir / "system-prompt"
    program.write_text(f"#!/bin/sh\ntouch {tmp_path / 'ran'}\n")
    program.chmod(0o755)
    chosen = tmp_path / "chosen.md"
    chosen.write_text("chosen")
    kernel("first", "-S", "chosen.md", env=home.env,
           later=[(seen("run_end"), lambda run: chosen.write_text("edited") and "second")])
    assert [r["system"] for r in home.requests] == ["chosen", "chosen"]
    assert not (tmp_path / "ran").exists()


def test_an_unreadable_s_file_stops_the_kernel_before_it_starts(kernel, home):
    run = kernel("hi", "-S", "missing.md", env=home.env, code=2)
    assert run.stderr == "akern: missing.md: No such file or directory\n"
    assert not home.requests and not run.session.exists()


def test_a_failing_system_prompt_program_stops_the_kernel_before_it_starts(kernel, home):
    program = executable(home.dir / "system-prompt", "#!/bin/sh\necho partial\necho broken >&2\nexit 3\n")
    run = kernel("hi", env=home.env, code=2)
    assert run.stderr == f"broken\nakern: {program}: exit 3\n"
    assert not home.requests and not run.session.exists()


STAY = '''#!/usr/bin/env python3
"""Answers each request line with "reply N", N counting this process's requests, in a result that stays."""
import json, os, sys
for n, line in enumerate(sys.stdin, 1):
    json.loads(line)
    with open(os.environ["STAY_LOG"], "a") as log:
        log.write(json.dumps({"pid": os.getpid(), "n": n}) + "\\n")
    if n == 2 and os.environ.get("STAY_DIES_ON_SECOND"):
        sys.exit(0)
    print(json.dumps({"type": "result", "stop": "end_turn", "stay": True,
                      "message": {"role": "assistant", "content": [{"type": "text", "text": f"reply {n}"}]}}), flush=True)
'''


def stay(home, tmp_path, **env):
    executable(home.dir / "providers" / "stay", STAY)
    return {**home.env, "STAY_LOG": str(tmp_path / "stay.jsonl"), **env}


def stay_log(tmp_path):
    return [json.loads(line) for line in (tmp_path / "stay.jsonl").read_text().splitlines()]


def test_a_provider_whose_result_stays_gets_the_next_request_on_the_same_stdin(kernel, home, tmp_path):
    run = kernel("first", env=stay(home, tmp_path), model="stay/x", later=[(seen("run_end"), "second")])
    assert run.replies == ["reply 1", "reply 2"]
    assert [e["reason"] for e in run.events_of("run_end")] == ["done", "done"]
    assert len({entry["pid"] for entry in stay_log(tmp_path)}) == 1


def test_a_provider_that_stayed_and_ends_before_answering_hands_the_request_to_a_new_one(kernel, home, tmp_path):
    run = kernel("first", env=stay(home, tmp_path, STAY_DIES_ON_SECOND="1"), model="stay/x",
                 later=[(seen("run_end"), "second")])
    assert run.replies == ["reply 1", "reply 1"]
    assert [e["reason"] for e in run.events_of("run_end")] == ["done", "done"]
    assert [entry["n"] for entry in stay_log(tmp_path)] == [1, 2, 1]
    assert len({entry["pid"] for entry in stay_log(tmp_path)}) == 2


def test_a_provider_that_does_not_stay_runs_once_per_turn(kernel, home):
    kernel("first", env=home.env, later=[(seen("run_end"), "second")])
    assert len(home.requests) == 2
