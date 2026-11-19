"""Fixtures for running akern-loop against the mock provider, and helpers for reading what it did."""

import json
import os
import re
import subprocess
import threading
import time
from pathlib import Path

import pytest

from invariants import event_violations, session_violations

REPO = Path(__file__).resolve().parent.parent
BUILD = Path(os.environ.get("AKERN_BUILD", REPO / "build"))


def tool(name, **input):
    """A prompt line that makes the mock model call one tool: `!@name {input}`."""
    return _calls("@" + name + " " + json.dumps(input, separators=(",", ":")))


def shell(*commands):
    """A prompt line that makes the mock model exec each command with `sh -c`, all at once."""
    return _calls(*commands)


def parallel(*lines):
    """Join prompt lines from tool() and shell() into one line whose calls run at once."""
    return _calls(*(line.removeprefix("!") for line in lines))


def send_now(content):
    """A control line that preempts whatever the kernel is doing to deliver `content`."""
    return json.dumps({"type": "send", "when": "now", "content": content})


def _calls(*calls):
    assert not any(";" in c for c in calls), "the mock provider splits prompts on ';'"
    return "!" + ";".join(calls)


def seen(type, n=1):
    """A condition for `later` and signal_when(): the kernel has sent at least `n` events of `type`."""
    return lambda run: len(run.events_of(type)) >= n


def seen_pending(n=1):
    """A condition: `n` calls have been delivered as still running."""
    return lambda run: sum(b["type"] == "tool_pending" for e in run.events_of("message")
                           for b in _blocks(e["message"])) >= n


def signal_when(*steps):
    """A `during` function: for each (condition, signal) step, waits for `condition(run)`, then sends `signal`."""
    def send(run):
        for ready, sig in steps:
            wait_for(lambda: ready(run))
            os.kill(run.pid, sig)
    return send


def wait_for(condition, timeout=5.0):
    """Poll until `condition()` returns something truthy, and return it."""
    deadline = time.monotonic() + timeout
    while not (value := condition()):
        assert time.monotonic() < deadline, "timed out waiting for a condition"
        time.sleep(0.05)
    return value


class Run:
    """One akern-loop run: its session log, its event stream, and readings of both."""

    def __init__(self, session, printing):
        self.session = session
        self.events_path = session.with_suffix(".events")
        self.stderr_path = session.with_suffix(".stderr")
        self.printing = printing

    @property
    def output(self):
        return self.events_path.read_text()

    @property
    def stderr(self):
        return self.stderr_path.read_text()

    @property
    def events(self):
        assert not self.printing, "with -p the kernel prints answers, not events"
        return _read_jsonl(self.events_path)

    @property
    def entries(self):
        return _read_jsonl(self.session)

    def events_of(self, type):
        return [e for e in self.events if e["type"] == type]

    def entries_of(self, type):
        return [e for e in self.entries if e["type"] == type]

    @property
    def messages(self):
        return [e["message"] for e in self.entries_of("message")]

    @property
    def calls(self):
        return [e["call"] for e in self.events_of("tool_start")]

    @property
    def tool_results(self):
        """Results delivered in the turn of their call."""
        return [b for m in self.messages for b in _blocks(m) if b["type"] == "tool_result"]

    @property
    def pending(self):
        """Calls still running when the next request went out."""
        return [b for m in self.messages for b in _blocks(m) if b["type"] == "tool_pending"]

    @property
    def result_texts(self):
        return [r["content"][0]["text"] for r in self.tool_results]

    @property
    def result_text(self):
        return self.result_texts[0]

    @property
    def result_json(self):
        return json.loads(self.result_text)

    @property
    def replies(self):
        return [_text(m) for m in self.messages if m["role"] == "assistant"]

    @property
    def streamed(self):
        return "".join(e["text"] for e in self.events_of("text_delta"))

    @property
    def job_reports(self):
        """Late results of pending calls, as (block, its text parsed as JSON when it is JSON) pairs."""
        late = [b for m in self.messages for b in _blocks(m) if b["type"] == "job_result"]
        return [(b, _json_or_text(b["content"][0]["text"])) for b in late]

    def violations(self):
        sessions = [self.session, *self.session.parent.glob("agents/*.jsonl")]
        found = [f"{s.name}: {v}" for s in sessions for v in session_violations(_read_jsonl(s))]
        if not self.printing:
            found += [f"{self.events_path.name}: {v}" for v in event_violations(self.events)]
        return found


class Kernel:
    """Runs akern-loop in a test's directory, one numbered session per run; sub-agents' go in agents/."""

    def __init__(self, dir, env):
        self.dir = dir
        (dir / "agents").mkdir()
        self.env = env
        self.runs = 0

    def __call__(self, prompt=None, *args, later=(), env=None, during=None, timeout=10, code=0):
        """Run the kernel with `prompt` as the first stdin line and wait for it to exit with `code`.

        later:  (when, line) pairs: each line is written `when` seconds after the one before, or,
                when `when` is a function, once it returns something truthy for the Run; a line
                that is a function is given the Run;
                stdin closes after the last.
        during: a function given the Run, called in a thread while the kernel runs.
        """
        self.runs += 1
        run = Run(self.dir / f"run{self.runs}.jsonl", printing="-p" in args)
        argv = ["akern-loop", "-m", "mock/echo", "-s", str(run.session), *args]
        with open(run.events_path, "w") as out, open(run.stderr_path, "w") as err:
            proc = subprocess.Popen(argv, cwd=self.dir, env={**self.env, "TMPDIR": str(self.dir), **(env or {})},
                                    stdin=subprocess.PIPE, stdout=out, stderr=err, text=True)
        run.pid = proc.pid
        threads = [_start(_feed, proc.stdin, run, [] if prompt is None else [(0, prompt)], later)]
        if during:
            threads.append(_start(during, run))
        try:
            exited = proc.wait(timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            pytest.fail(f"akern-loop hung for {timeout}s; events in {run.events_path}")
        for t in threads:
            t.join(timeout)
            t.reraise()
        assert exited == code, f"akern-loop exited {exited}: {run.stderr}"
        assert not (found := run.violations()), "invariants broken:\n" + "\n".join(found)
        return run


@pytest.fixture(scope="session")
def akern_env():
    missing = [p for p in ("bin/akern-loop", "bin/akern-edit", "providers/mock") if not (BUILD / p).exists()]
    if missing:
        pytest.exit(f"not built ({', '.join(missing)} missing): run make first", returncode=2)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("AKERN_") and k != "MOCK_SCRIPT"}
    env["AKERN_HOME"] = str(BUILD)
    env["PATH"] = os.pathsep.join([str(BUILD / "bin"), str(REPO / "tests" / "bin"), env["PATH"]])
    return env


@pytest.fixture
def kernel(tmp_path, akern_env):
    return Kernel(tmp_path, akern_env)


class Home:
    """An AKERN_HOME of a test's own: the built providers behind a mock that records each request."""

    def __init__(self, dir):
        self.dir = dir
        self.requests_path = dir / "requests.jsonl"
        (dir / "providers").mkdir(parents=True)
        for built in (BUILD / "providers").iterdir():
            (dir / "providers" / built.name).symlink_to(built)
        self.mock("")
        self.env = {"AKERN_HOME": str(dir)}

    def mock(self, script):
        """Runs `script`, then the built mock on the request, which is appended to requests.jsonl first."""
        path = self.dir / "providers" / "mock"
        path.unlink(missing_ok=True)
        path.write_text(f'#!/bin/sh\n{script}\nrequest=$(cat)\nprintf "%s\\n" "$request" >> "{self.requests_path}"\n'
                        f'printf "%s" "$request" | exec "{BUILD / "providers" / "mock"}"\n')
        path.chmod(0o755)

    @property
    def requests(self):
        return _read_jsonl(self.requests_path)


@pytest.fixture
def home(tmp_path):
    return Home(tmp_path / "home")


@pytest.fixture
def script(tmp_path):
    """Writes a MOCK_SCRIPT, one JSON line per object, and returns the env that selects it."""
    def write(*lines):
        path = tmp_path / "script.jsonl"
        path.write_text("".join(json.dumps(line) + "\n" for line in lines))
        return {"MOCK_SCRIPT": str(path)}
    return write


def _feed(stdin, run, *schedules):
    try:
        for when, line in (item for schedule in schedules for item in schedule):
            if callable(when):
                wait_for(lambda: when(run))
            else:
                time.sleep(when)
            stdin.write((line(run) if callable(line) else line) + "\n")
            stdin.flush()
    except BrokenPipeError:
        pass
    finally:
        try:
            stdin.close()
        except BrokenPipeError:
            pass


class _Thread(threading.Thread):
    def run(self):
        self.error = None
        try:
            super().run()
        except BaseException as e:
            self.error = e

    def reraise(self):
        if self.error:
            raise self.error


def _start(target, *args):
    thread = _Thread(target=target, args=args, daemon=True)
    thread.start()
    return thread


def _read_jsonl(path):
    if not path.exists():
        return []
    lines = path.read_text().splitlines(keepends=True)
    return [json.loads(line) for line in lines if line.endswith("\n")]


def _json_or_text(text):
    try:
        return json.loads(text)
    except ValueError:
        return text


def _blocks(message):
    content = message["content"]
    return content if isinstance(content, list) else [{"type": "text", "text": content}]


def _text(message):
    return "".join(b["text"] for b in _blocks(message) if b["type"] == "text")
