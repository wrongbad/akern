import base64
import json
import shlex

import pytest

from conftest import executable, parallel, tool

TINY_PNG = b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\0\x02\0\0\0\x03\x08\x02\0\0\0"


def test_cat_writes_and_appends_stdin_exactly_and_akern_edit_replaces_text_exactly(kernel, tmp_path):
    path = tmp_path / "f.txt"
    kernel(tool("shell", command=f"cat > {shlex.quote(str(path))}", stdin='alpha "$x" `y` %s\nbeta\ngamma\n'))
    kernel(tool("shell", command=f"cat >> {shlex.quote(str(path))}", stdin="delta\n"))
    edit = kernel(tool("shell", command=shlex.join(["akern-edit", str(path), "beta\ngamma", "BETA\nGAMMA"])))
    assert path.read_text() == 'alpha "$x" `y` %s\nBETA\nGAMMA\ndelta\n'
    assert "First change at line 2" in edit.result_text
    assert path.stat().st_mode & 0o777 == 0o644


def test_exec_stdin_reaches_the_program(kernel):
    run = kernel(tool("shell", command="cat", stdin="piped"))
    assert run.shell_result["output"] == "piped"


@pytest.mark.parametrize("head, media_type", [
    (TINY_PNG, "image/png"),
    (b"\xff\xd8\xff\xe0\0\x10JFIF", "image/jpeg"),
    (b"GIF89a\x01\0\x01\0", "image/gif"),
    (b"RIFF\x1a\0\0\0WEBPVP8 ", "image/webp"),
])
def test_an_image_on_stdout_comes_back_as_an_image_block(kernel, tmp_path, head, media_type):
    image = tmp_path / "image"
    image.write_bytes(head)
    run = kernel(tool("shell", command=f"cat {shlex.quote(str(image))}"))
    [block] = run.tool_results[0]["content"]
    assert block == {"type": "image", "source": {"type": "base64", "media_type": media_type,
                                                 "data": base64.b64encode(head).decode()}}


def test_stderr_before_an_image_makes_it_binary_output_not_shown(kernel, tmp_path):
    image = tmp_path / "image"
    image.write_bytes(TINY_PNG)
    run = kernel(tool("shell", command=f"echo note >&2 && cat {shlex.quote(str(image))}"))
    out = f"{run.session}.d/calls/{run.calls[0]}.out"
    assert run.result_text == f"[exit 0; {len(TINY_PNG) + 5} bytes of binary output not shown, all of it in {out}]"


def test_stderr_sent_elsewhere_keeps_the_image(kernel, tmp_path):
    image = tmp_path / "image"
    image.write_bytes(TINY_PNG)
    run = kernel(tool("shell", command=f"(echo note >&2 && cat {shlex.quote(str(image))}) 2>/dev/null"))
    assert [b["type"] for b in run.tool_results[0]["content"]] == ["image"]


def test_stdout_and_stderr_arrive_in_the_order_written(kernel):
    run = kernel(tool("shell", command="echo 1 && echo 2 >&2 && echo 3 && echo 4 >&2"))
    assert run.result_text == "[exit 0]\n1\n2\n3\n4\n"


def test_an_image_from_a_failed_program_is_not_shown(kernel, tmp_path):
    image = tmp_path / "image"
    image.write_bytes(TINY_PNG)
    run = kernel(tool("shell", command=f"cat {shlex.quote(str(image))} && exit 1"))
    out = f"{run.session}.d/calls/{run.calls[0]}.out"
    assert run.result_text == f"[exit 1; {len(TINY_PNG)} bytes of image/png not shown, all of it in {out}]"
    assert run.tool_results[0]["is_error"] is True


def test_a_pdf_on_stdout_is_a_document_named_by_stdout_name(kernel):
    run = kernel(tool("shell", command="printf %s %PDF-", stdout_name="report.pdf"))
    assert run.tool_results[0]["content"][0] == {"type": "document", "name": "report.pdf", "source": {
        "type": "base64", "media_type": "application/pdf", "data": base64.b64encode(b"%PDF-").decode()}}


def test_a_pdf_without_stdout_name_has_no_name(kernel):
    run = kernel(tool("shell", command="printf %s %PDF-"))
    assert "name" not in run.tool_results[0]["content"][0]


def test_text_that_starts_like_no_known_format_stays_text(kernel):
    run = kernel(tool("shell", command="printf %s 'GIF8 is not a header'"))
    assert run.shell_result["output"] == "GIF8 is not a header"


def test_a_missing_program_fails_with_127(kernel):
    run = kernel(tool("shell", command="no-such-program"))
    assert (run.shell_result["exit"], "no-such-program" in run.shell_result["output"]) == (127, True)


def test_a_shim_on_path_sees_every_call_and_can_refuse_one(kernel, tmp_path):
    shims, log = tmp_path / "shims", tmp_path / "calls.log"
    shims.mkdir()
    for name in ("bash", "akern-edit"):
        executable(shims / name, f"""#!/bin/sh
echo "{name} $*" >> "$SHIM_LOG"
case "$1" in *secret*) echo "{name}: $1: refused" >&2; exit 1;; esac
PATH="${{PATH#*:}}" exec {name} "$@"
""")
    secret = tmp_path / "secret.txt"
    secret.write_text("hush\n")
    run = kernel(parallel(tool("shell", command="echo ok"), tool("edit", path=str(secret), old="hush", new="shout")),
                 env={"PATH": f"{shims}:{kernel.env['PATH']}", "SHIM_LOG": str(log)})
    assert run.result_texts[0] == "[exit 0]\nok\n"
    assert run.result_texts[1] == f"akern-edit: {secret}: refused"
    assert secret.read_text() == "hush\n"
    assert sorted(log.read_text().splitlines()) == [f"akern-edit {secret} hush shout", "bash -c echo ok"]


def test_the_tools_sent_are_tools_json_from_akern_home_compacted_without_their_commands(kernel, home):
    (home.dir / "tools.json").write_text('[\n  {"name": "shell", "description": "Run  it.",\n   "input_schema": {"type": "object"},\n'
                                         '   "program": "bash"}\n]\n')
    kernel("hi", env=home.env)
    assert home.requests[0]["tools"] == [{"name": "shell", "description": "Run  it.", "input_schema": {"type": "object"}}]


def test_a_command_of_your_own_runs_in_place_of_the_shipped_program(kernel, home, tmp_path):
    wrapper = executable(tmp_path / "wrapped", '#!/bin/sh\necho "wrapped $*" >&2\nexec bash "$@"\n')
    tools = json.loads((home.dir / "system" / "tools.json").read_text())
    tools[0]["program"] = str(wrapper)
    (home.dir / "tools.json").write_text(json.dumps(tools))
    run = kernel(tool("shell", command="echo hi"), env=home.env)
    assert run.result_text == "wrapped -c echo hi\nhi\n"


def test_a_tool_the_kernel_cannot_run_stops_akern_at_startup(kernel, home):
    (home.dir / "tools.json").write_text('[{"name": "shout", "program": "tr"}]')
    run = kernel("hi", env=home.env, code=2)
    assert run.stderr == f"akern: {home.dir}/tools.json: tool 1 is not shell, job or edit\n"


def test_a_tools_json_that_is_not_an_array_stops_akern_at_startup(kernel, home):
    (home.dir / "tools.json").write_text('{"name": "exec"}')
    run = kernel("hi", env=home.env, code=2)
    assert run.stderr == f"akern: {home.dir}/tools.json: not a JSON array\n"


def test_edit_replaces_exact_text_with_no_escaping_and_shows_the_change(kernel, tmp_path):
    path = tmp_path / "f.py"
    path.write_text('a = 1\nprint("x \\n $y")\nb = 2\n')
    run = kernel(tool("edit", path=str(path), old='print("x \\n $y")', new='print(\'x\')\nprint("z")'))
    assert path.read_text() == 'a = 1\nprint(\'x\')\nprint("z")\nb = 2\n'
    assert run.result_text.startswith(f"Edited {path}: replaced 1 occurrence. First change at line 2:\n")
    assert run.tool_results[0].get("is_error") is not True


def test_edit_refuses_text_that_matches_twice_unless_all(kernel, tmp_path):
    path = tmp_path / "f.txt"
    path.write_text("x\nx\n")
    run = kernel(tool("edit", path=str(path), old="x", new="y"))
    assert run.result_text == f"akern-edit: {path}: OLD text matches 2 times"
    assert run.tool_results[0]["is_error"] is True
    kernel(tool("edit", path=str(path), old="x", new="y", all=True))
    assert path.read_text() == "y\ny\n"


def test_a_missing_required_field_runs_nothing(kernel):
    run = kernel(tool("edit", old="a", new="b"))
    assert run.result_text == "path is required"
