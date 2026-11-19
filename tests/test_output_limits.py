from conftest import tool


def test_stdout_past_100_kb_is_cut_and_spilled_whole_to_a_file(kernel, tmp_path):
    run = kernel(tool("exec", argv=["sh", "-c", "yes abcdefghi | head -c 150000"]))
    result = run.result_json
    assert len(result["stdout"]) == 100_000
    assert result["stdout_total"] == 150_000
    assert (tmp_path / result["stdout_file"]).stat().st_size == 150_000
    assert "stderr_total" not in result


def test_stdout_limit_and_stderr_limit_spill_each_stream_to_its_own_file(kernel, tmp_path):
    run = kernel(tool("exec", argv=["sh", "-c", "yes abcdefghi | head -c 25000 && yes e | head -c 3000 >&2"],
                      stdout_limit=1000, stderr_limit=500))
    result = run.result_json
    assert len(result["stdout"]) == 1000
    assert len(result["stderr"]) == 500
    assert result["stdout_total"] == 25_000
    assert result["stderr_total"] == 3000
    assert (tmp_path / result["stdout_file"]).stat().st_size == 25_000
    assert (tmp_path / result["stderr_file"]).stat().st_size == 3000
    assert result["stdout_file"] != result["stderr_file"]


def test_a_spill_file_that_cannot_be_written_is_reported_with_the_cut_output(kernel, tmp_path):
    calls = tmp_path / "run1.jsonl.d" / "calls"
    calls.mkdir(parents=True)
    calls.chmod(0o555)
    run = kernel(tool("exec", argv=["sh", "-c", "yes abcdefghi | head -c 2000"], stdout_limit=100))
    result = run.result_json
    assert (len(result["stdout"]), result["stdout_total"]) == (100, 2000)
    assert result["stdout_file_error"] == "Permission denied"
    assert not (tmp_path / result["stdout_file"]).exists()
