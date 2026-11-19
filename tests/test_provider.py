from conftest import BUILD


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


def test_the_system_prompt_is_system_md_in_akern_home_then_in_system_unless_s_names_a_file(kernel, home, tmp_path):
    (home.dir / "system.md").write_text("from home")
    (tmp_path / "chosen.md").write_text("chosen")
    kernel("hi", env=home.env)
    kernel("hi", "-S", "chosen.md", env=home.env)
    (home.dir / "system.md").unlink()
    kernel("hi", env=home.env)
    (home.dir / "system" / "system.md").unlink()
    kernel("hi", env=home.env)
    assert [r["system"] for r in home.requests] == ["from home", "chosen", (BUILD / "system" / "system.md").read_text(), None]
