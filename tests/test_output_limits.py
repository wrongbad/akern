from conftest import tool


def test_output_past_100_kb_is_cut_and_spilled_whole_to_a_file(kernel, tmp_path):
    run = kernel(tool("shell", command="yes abcdefghi | head -c 150000"))
    result = run.shell_result
    assert len(result["output"]) == 100_000
    assert result["total"] == 150_000
    assert (tmp_path / result["file"]).stat().st_size == 150_000


def test_output_limit_counts_stdout_and_stderr_together_and_the_file_keeps_their_order(kernel, tmp_path):
    run = kernel(tool("shell", command="yes o | head -c 2000 && yes e | head -c 1000 >&2", output_limit=500))
    result = run.shell_result
    assert (len(result["output"]), result["total"]) == (500, 3000)
    assert (tmp_path / result["file"]).read_text() == "o\n" * 1000 + "e\n" * 500


def test_a_spill_file_that_cannot_be_written_is_reported_with_the_cut_output(kernel, tmp_path):
    calls = tmp_path / "run1.jsonl.d" / "calls"
    calls.mkdir(parents=True)
    calls.chmod(0o555)
    run = kernel(tool("shell", command="yes abcdefghi | head -c 2000", output_limit=100))
    result = run.shell_result
    assert (len(result["output"]), result["total"]) == (100, 2000)
    assert result["file_error"] == "Permission denied"
    assert not (tmp_path / result["file"]).exists()


def test_the_header_says_where_the_cut_output_is(kernel):
    run = kernel(tool("shell", command="printf 'abcdef'", output_limit=3))
    header, kept = run.result_text.split("\n")
    assert kept == "abc"
    assert header.startswith("[exit 0; cut at 3 of 6 bytes, all of it in ") and header.endswith(".out]")


def test_a_command_with_no_output_gets_the_header_alone(kernel):
    run = kernel(tool("shell", command="true"))
    assert run.result_text == "[exit 0]"


def test_output_that_is_not_text_is_not_shown(kernel):
    run = kernel(tool("shell", command="printf 'a\\0b' && exit 1"))
    assert run.result_text == "[exit 1; 3 bytes of binary output not shown]"


def test_text_with_a_few_bytes_outside_utf8_is_shown(kernel):
    run = kernel(tool("shell", command="printf 'caf\\xe9 au lait\\n'"))
    assert run.result_text == "[exit 0]\ncaf� au lait\n"
