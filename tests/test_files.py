import base64

from conftest import tool

TINY_PNG = b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\0\x02\0\0\0\x03\x08\x02\0\0\0"


def test_write_creates_and_appends_and_akern_edit_replaces_text_exactly(kernel, tmp_path):
    path = tmp_path / "f.txt"
    kernel(tool("write", path=str(path), content='alpha "$x" `y` %s\nbeta\ngamma\n'))
    kernel(tool("write", path=str(path), content="delta\n", append=True))
    edit = kernel(tool("exec", argv=["akern-edit", str(path), "beta\ngamma", "BETA\nGAMMA"]))
    assert path.read_text() == 'alpha "$x" `y` %s\nBETA\nGAMMA\ndelta\n'
    assert "First change at line 2" in edit.result_text
    assert path.stat().st_mode & 0o777 == 0o644


def test_read_gives_exact_lines_with_a_note_to_read_on(kernel, tmp_path):
    path = tmp_path / "f.txt"
    path.write_text("alpha\nBETA\nGAMMA\ndelta\n")
    run = kernel(tool("read", path=str(path), offset=2, limit=2))
    assert run.result_text == "BETA\nGAMMA\n[lines 2-3 of 4; read on with offset 4]"


def test_write_names_a_missing_directory(kernel, tmp_path):
    path = tmp_path / "no" / "such" / "f.txt"
    run = kernel(tool("write", path=str(path), content="x"))
    assert run.result_text == f"{path}: directory does not exist"
    assert run.tool_results[0]["is_error"] is True
    assert not path.parent.exists()


def test_exec_stdin_reaches_the_program(kernel):
    run = kernel(tool("exec", argv=["cat"], stdin="piped"))
    assert run.result_json["stdout"] == "piped"


def test_stdout_type_image_returns_an_image_block(kernel, tmp_path):
    png = tmp_path / "tiny.png"
    png.write_bytes(TINY_PNG)
    run = kernel(tool("exec", argv=["cat", str(png)], stdout_type="image/png"))
    image = run.tool_results[0]["content"][0]
    assert image["type"] == "image"
    assert image["source"]["data"] == base64.b64encode(TINY_PNG).decode()


def test_read_returns_a_file_as_the_media_type_the_call_names(kernel, tmp_path):
    png = tmp_path / "tiny.png"
    png.write_bytes(TINY_PNG)
    run = kernel(tool("read", path=str(png), type="image/png"))
    image = run.tool_results[0]["content"][0]
    assert image["type"] == "image"
    assert image["source"] == {"type": "base64", "media_type": "image/png", "data": base64.b64encode(TINY_PNG).decode()}


def test_read_without_a_type_refuses_a_binary_file(kernel, tmp_path):
    png = tmp_path / "tiny.png"
    png.write_bytes(TINY_PNG)
    run = kernel(tool("read", path=str(png)))
    assert run.result_text == f"{png} is a binary file ({len(TINY_PNG)} bytes)"


def test_read_cuts_a_long_line_at_a_character_and_counts_the_rest_in_characters(kernel, tmp_path):
    path = tmp_path / "f.txt"
    path.write_text("é" * 2500 + "\nnext\n")
    run = kernel(tool("read", path=str(path)))
    assert run.result_text.startswith("é" * 2000 + "… (500 more characters)\nnext\n")
    assert "�" not in run.result_text
