"""The akern-cli front end's default model: AKERN_MODEL, then $AKERN_HOME/model, then akern-model's picker on the terminal."""

import contextlib
import os
import pty
import select
import signal
import subprocess

import pytest

from conftest import BUILD, PATIENCE, executable

FAKE = """#!/bin/sh
case $1 in --models) printf '%s\\n' '{"id":"a","context":5}' '{"id":"b"}'; exit ;; esac
head -n 1 | jq -c '"model " + .model | {type: "text_delta", text: .}, {type: "result", stop: "end_turn",
    message: {role: "assistant", content: [{type: "text", text: .}]}}'
"""
BROKEN = """#!/bin/sh
echo '{"type":"error","kind":"auth","message":"BROKEN_KEY is not set","status":0}'
exit 1
"""
SILENT_FAILURE = "#!/bin/sh\nexit 3\n"
QUIET = "#!/bin/sh\nexit 0\n"


@pytest.fixture
def home(tmp_path):
    home = tmp_path / "home"
    (home / "providers").mkdir(parents=True)
    (home / "system" / "providers").mkdir(parents=True)
    (home / "system" / "render.jq").symlink_to(BUILD / "system" / "render.jq")
    (home / "system" / "tools.json").symlink_to(BUILD / "system" / "tools.json")
    for layer in (home, home / "system"):
        (layer / "providers" / "mock").symlink_to(BUILD / "system" / "providers" / "mock")
    for name, text in {"fake": FAKE, "broken": BROKEN, "silent": SILENT_FAILURE, "quiet": QUIET}.items():
        executable(home / "providers" / name, text)
    return home


@pytest.fixture
def env(akern_env, home, tmp_path):
    return {**akern_env, "AKERN_HOME": str(home), "TMPDIR": str(tmp_path)}


def detached(argv, env):
    """Runs argv in a session of its own, with no controlling terminal."""
    return subprocess.run(argv, env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                          start_new_session=True, timeout=PATIENCE)


class Terminal:
    """A program on a pty: what it has shown so far, and lines typed into it."""

    def __init__(self, argv, env):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execvpe(argv[0], argv, env)
        self.screen = bytearray()

    @property
    def text(self):
        return self.screen.decode(errors="replace").replace("\r\n", "\n")

    def read(self):
        assert select.select([self.fd], [], [], PATIENCE)[0], f"terminal idle: {self.text!r}"
        with contextlib.suppress(OSError):
            chunk = os.read(self.fd, 4096)
            self.screen.extend(chunk)
            return chunk
        return b""

    def until(self, text, times=1):
        while self.text.count(text) < times:
            assert self.read(), f"terminal closed before {text!r}: {self.text!r}"

    def type(self, line):
        os.write(self.fd, line.encode() + b"\r")

    def finish(self):
        while self.read():
            pass
        _, status = os.waitpid(self.pid, 0)
        self.pid = None
        return os.waitstatus_to_exitcode(status)

    def close(self):
        os.close(self.fd)
        if self.pid:
            with contextlib.suppress(OSError):
                os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)


@pytest.fixture
def terminal():
    opened = []

    def start(argv, env):
        opened.append(Terminal(argv, env))
        return opened[-1]
    yield start
    for t in opened:
        t.close()


PROVIDERS = ("  1) broken  \x1b[2m(unavailable: BROKEN_KEY is not set)\x1b[0m\n"
             "  2) fake\n"
             "  3) mock\n"
             "  4) quiet\n"
             "  5) silent  \x1b[2m(unavailable)\x1b[0m\n"
             "Provider [1-5]: ")


def test_the_environment_beats_the_file(env, home):
    (home / "model").write_text("fake/from-file\n")
    run = detached(["akern-cli", "hello"], {**env, "AKERN_MODEL": "fake/from-env"})
    assert run.returncode == 0, run.stderr
    assert "model from-env" in run.stdout


def test_the_file_beats_the_picker(env, home):
    (home / "model").write_text("fake/from-file\nignored\n")
    run = detached(["akern-cli", "hello"], env)
    assert run.returncode == 0, run.stderr
    assert "model from-file" in run.stdout


def test_with_neither_the_front_end_asks_and_without_a_terminal_it_fails(env, home):
    run = detached(["akern-cli", "hello"], env)
    assert (run.returncode, run.stdout, run.stderr) == (1, "", "akern-model: no model set\n")
    assert not (home / "model").exists()


def test_without_a_terminal_and_with_a_default_the_picker_says_there_is_no_terminal(env, home):
    (home / "model").write_text("fake/a\n")
    run = detached(["akern-model"], env)
    assert (run.returncode, run.stderr) == (1, "akern-model: no terminal\n")
    assert (home / "model").read_text() == "fake/a\n"


def test_a_model_given_is_saved_and_printed_without_asking(env, home):
    run = detached(["akern-model", "fake/b"], env)
    assert (run.returncode, run.stdout, run.stderr) == (0, "fake/b\n", "")
    assert (home / "model").read_text() == "fake/b\n"


@pytest.mark.parametrize("args", [["nope"], ["a/b", "c"], ["/b"]])
def test_anything_but_one_provider_slash_model_is_a_usage_error(env, home, args):
    run = detached(["akern-model", *args], env)
    assert run.returncode == 2 and run.stderr.startswith("usage: akern-model")
    assert not (home / "model").exists()


def test_help_prints_one_usage_line(env):
    run = detached(["akern-model", "--help"], env)
    assert run.returncode == 0 and run.stdout.startswith("usage: akern-model") and run.stdout.count("\n") == 1


def test_the_picker_lists_every_provider_refuses_an_unavailable_one_and_saves_the_model_chosen(env, home, terminal):
    t = terminal(["akern-model"], env)
    t.until("Provider [1-5]: ")
    assert t.text == PROVIDERS
    t.type("1")
    t.until("broken is unavailable: BROKEN_KEY is not set\n")
    t.until(PROVIDERS, times=2)
    t.type("9")
    t.type("2")
    t.until("Model [1-3]: ")
    assert t.text.endswith("  1) a\n  2) b\n  3) (type a model id)\nModel [1-3]: ")
    t.type("2")
    assert t.finish() == 0
    assert t.text.endswith("fake/b\n")
    assert (home / "model").read_text() == "fake/b\n"


def test_the_picker_takes_a_typed_id(env, home, terminal):
    t = terminal(["akern-model"], env)
    t.until("Provider [1-5]: ")
    t.type("2")
    t.until("Model [1-3]: ")
    t.type("3")
    t.until("Model id: ")
    t.type("c")
    assert t.finish() == 0
    assert (home / "model").read_text() == "fake/c\n"


def test_a_provider_that_lists_nothing_is_asked_for_an_id(env, home, terminal):
    t = terminal(["akern-model"], env)
    t.until("Provider [1-5]: ")
    t.type("4")
    t.until("Model id: ")
    assert "Model [" not in t.text
    t.type("x")
    assert t.finish() == 0
    assert (home / "model").read_text() == "quiet/x\n"


def test_the_picker_asks_even_with_a_default_and_ctrl_d_keeps_it(env, home, terminal):
    (home / "model").write_text("fake/a\n")
    t = terminal(["akern-model"], env)
    t.until("Provider [1-5]: ")
    os.write(t.fd, b"\x04")
    assert t.finish() == 1
    assert (home / "model").read_text() == "fake/a\n"


def test_with_no_provider_ready_the_picker_shows_why_and_fails(env, home, terminal):
    for name in ("fake", "mock", "quiet"):
        (home / "providers" / name).unlink()
    (home / "system" / "providers" / "mock").unlink()
    t = terminal(["akern-model"], env)
    assert t.finish() == 1
    assert t.text == ("  1) broken  \x1b[2m(unavailable: BROKEN_KEY is not set)\x1b[0m\n"
                      "  2) silent  \x1b[2m(unavailable)\x1b[0m\n"
                      "akern-model: no provider is ready\n")


def test_a_bare_akern_cli_asks_for_a_model_the_first_time_then_runs_with_it(env, home, terminal):
    t = terminal(["akern-cli", "hello"], env)
    t.until("Provider [1-5]: ")
    t.type("2")
    t.until("Model [1-3]: ")
    t.type("1")
    t.until("model a")
    os.write(t.fd, b"\x04")
    assert t.finish() == 0
    assert (home / "model").read_text() == "fake/a\n"
