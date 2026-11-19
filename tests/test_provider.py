from conftest import seen


def answer(text):
    return {"type": "result", "stop": "end_turn", "message": {"role": "assistant", "content": [{"type": "text", "text": text}]}}


def test_a_result_after_a_provider_error_line_wins_and_the_run_ends_done(kernel, script):
    run = kernel("hi", env=script({"type": "error", "kind": "overloaded", "message": "retrying"}, answer("recovered")))
    [end] = run.events_of("run_end")
    assert (end["reason"], end["error"]) == ("done", None)
    assert run.replies == ["recovered"]


def test_a_provider_line_longer_than_max_line_bytes_is_dropped_whole(kernel, script):
    run = kernel("hi", "--max-line-bytes", "1000", env=script({"type": "text_delta", "text": "x" * 200_000}, answer("ok")))
    assert not run.events_of("error")
    assert run.streamed == ""


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


def test_the_system_prompt_is_the_output_of_the_first_prompt_program_and_null_when_empty_or_missing(kernel, home):
    program = home.dir / "prompt"
    program.write_text("#!/bin/sh\necho from home\n")
    program.chmod(0o755)
    kernel("hi", env=home.env)
    program.write_text("#!/bin/sh\n")
    kernel("hi", env=home.env)
    program.unlink()
    (home.dir / "system" / "prompt").unlink()
    kernel("hi", env=home.env)
    assert [r["system"] for r in home.requests] == ["from home\n", None, None]


def test_s_reads_its_file_once_and_skips_the_prompt_program(kernel, home, tmp_path):
    program = home.dir / "prompt"
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


def test_a_failing_prompt_program_stops_the_kernel_before_it_starts(kernel, home):
    program = home.dir / "prompt"
    program.write_text("#!/bin/sh\necho partial\necho broken >&2\nexit 3\n")
    program.chmod(0o755)
    run = kernel("hi", env=home.env, code=2)
    assert run.stderr == f"broken\nakern: {program}: exit 3\n"
    assert not home.requests and not run.session.exists()
