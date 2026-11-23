"""Fixtures for running akern against the mock provider, and helpers for reading what it did."""

import hashlib
import json
import os
import re
import shutil
import subprocess
import threading
import time
from pathlib import Path

import pytest

from invariants import context_order, event_violations, log_violations, session_violations

REPO = Path(__file__).resolve().parent.parent
BUILD = Path(os.environ.get("AKERN_BUILD", REPO / "build"))
SCRIPTS = BUILD / "test-scripts"

# Seconds to wait for anything before calling it hung. No test measures speed: a loaded
# machine is slow, not wrong, and these budgets exist only so that a hang fails.
PATIENCE = 30.0


def tool(name, **input):
    """A prompt line that makes the mock model call one tool: `!@name {input}`."""
    return _calls("@" + name + " " + json.dumps(input, separators=(",", ":")))


def shell(*commands):
    """A prompt line that makes the mock model make one shell call per command, all at once."""
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
    """A condition: `n` calls have become jobs (still running when a later turn started or the run ended)."""
    return lambda run: len(pending_calls(run.events)) >= n


def pending_calls(events):
    """The tool_start events of calls still running, without a result, when a later turn started
    or their run ended: the calls the model was answered pending."""
    started, answered, pending = [], set(), []
    for e in events:
        if e["type"] == "tool_start":
            started.append(e)
        elif e["type"] == "block":
            answered.add(e["block"].get("tool_use_id"))
        elif e["type"] in ("turn_start", "run_end"):
            pending += [s for s in started if s["call"] not in answered and s not in pending]
    return pending


def signal_when(*steps):
    """A `during` function: for each (condition, signal) step, waits for `condition(run)`, then sends `signal`."""
    def send(run):
        for ready, sig in steps:
            wait_for(lambda: ready(run))
            os.kill(run.pid, sig)
    return send


def executable(path, text):
    """Makes `path` a program that is `text`: a hard link to a kept copy of the same text, as macOS
    vets each new executable file on its first run (about 0.4 s) and a kept copy is vetted once.
    The copy is read-only, so writing through the link fails instead of changing it for others."""
    kept = SCRIPTS / hashlib.sha256(text.encode()).hexdigest()[:20]
    if not kept.exists():
        SCRIPTS.mkdir(exist_ok=True)
        fresh = kept.with_suffix(f".{os.getpid()}")
        fresh.write_text(text)
        fresh.chmod(0o555)
        os.replace(fresh, kept)
    path.unlink(missing_ok=True)
    try:
        os.link(kept, path)
    except OSError:
        shutil.copy(kept, path)
    return path


def wait_for(condition, timeout=PATIENCE):
    """Poll until `condition()` returns something truthy, and return it."""
    deadline = time.monotonic() + timeout
    while not (value := condition()):
        assert time.monotonic() < deadline, "timed out waiting for a condition"
        time.sleep(0.05)
    return value


class Run:
    """One akern run: its session log, its event stream, and readings of both."""

    def __init__(self, session, printing, name=None):
        self.session = session
        name = name or session
        self.events_path = name.with_suffix(".events")
        self.stderr_path = name.with_suffix(".stderr")
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
        return _read_jsonl(self.session, skip_invalid=True)

    def events_of(self, type):
        return [e for e in self.events if e["type"] == type]

    def entries_of(self, type):
        return [e for e in self.entries if e["type"] == type]

    @property
    def messages(self):
        """The messages in context order (a reply sits at its turn_start)."""
        return [e["message"] for e in context_order(self.entries)]

    @property
    def calls(self):
        return [e["call"] for e in self.events_of("tool_start")]

    @property
    def tool_results(self):
        """Results delivered in the turn of their call (a late one also has a status)."""
        return [b for m in self.messages for b in _blocks(m) if b["type"] == "tool_result" and "status" not in b]

    @property
    def pending(self):
        """The tool_start events of calls still running when the next request went out."""
        return pending_calls(self.events)

    @property
    def result_texts(self):
        return [r["content"][0]["text"] for r in self.tool_results]

    @property
    def result_text(self):
        return self.result_texts[0]

    @property
    def shell_result(self):
        return parse_shell(self.result_text)

    @property
    def replies(self):
        return [_text(m) for m in self.messages if m["role"] == "assistant"]

    @property
    def streamed(self):
        return "".join(e["text"] for e in self.events_of("text_delta"))

    @property
    def job_reports(self):
        """Late results of pending calls, as (block, its text read by parse_shell when it has a header) pairs."""
        late = [b for m in self.messages for b in _blocks(m) if b["type"] == "tool_result" and "status" in b]
        return [(b, _shell_or_text(b["content"][0]["text"])) for b in late]

    def violations(self):
        sessions = [self.session, *self.session.parent.glob("agents/*.jsonl")]
        found = [f"{s.name}: {v}" for s in sessions for v in session_violations(_read_jsonl(s, skip_invalid=True))]
        if not self.printing:
            found += [f"{self.events_path.name}: {v}" for v in event_violations(self.events)]
            found += [f"{self.session.name}: {v}" for v in log_violations(self.entries, self.events)]
        return found


class Kernel:
    """Runs akern in a test's directory, one numbered session per run; sub-agents' go in agents/."""

    def __init__(self, dir, env):
        self.dir = dir
        (dir / "agents").mkdir()
        self.env = env
        self.own = {"TMPDIR": str(dir), "XDG_STATE_HOME": str(dir / "state")}  # akern-acp keeps sessions there
        self.runs = 0

    def __call__(self, prompt=None, *args, later=(), env=None, during=None, timeout=PATIENCE, code=0,
                 session=None, model="mock/echo"):
        """Run the kernel with `prompt` as the first stdin line and wait for it to exit with `code`.

        later:  (when, line) pairs: each line is written `when` seconds after the one before, or,
                when `when` is a function, once it returns something truthy for the Run; a line
                that is a function is given the Run;
                stdin closes after the last.
        during: a function given the Run, called in a thread while the kernel runs.
        session: an earlier run's session file, to resume it; model: None leaves out -m.
        """
        self.runs += 1
        name = self.dir / f"run{self.runs}.jsonl"
        run = Run(session or name, printing="-p" in args, name=name)
        argv = ["akern", *(["-m", model] if model else []), "-s", str(run.session), *args]
        with open(run.events_path, "w") as out, open(run.stderr_path, "w") as err:
            proc = subprocess.Popen(argv, cwd=self.dir, env={**self.env, **self.own, **(env or {})},
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
            pytest.fail(f"akern hung for {timeout}s; events in {run.events_path}")
        for t in threads:
            t.join(timeout)
            t.reraise()
        assert exited == code, f"akern exited {exited}: {run.stderr}"
        assert not (found := run.violations()), "invariants broken:\n" + "\n".join(found)
        return run


@pytest.fixture(scope="session")
def akern_env():
    missing = [p for p in ("bin/akern", "bin/akern-edit", "system/providers/mock") if not (BUILD / p).exists()]
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
    """An AKERN_HOME of a test's own: the built system/ behind a user-layer mock that records each request."""

    def __init__(self, dir):
        self.dir = dir
        self.requests_path = dir / "requests.jsonl"
        (dir / "system").mkdir(parents=True)
        for built in (BUILD / "system").iterdir():
            (dir / "system" / built.name).symlink_to(built)
        (dir / "providers").mkdir()
        self.mock("")
        self.env = {"AKERN_HOME": str(dir)}

    def mock(self, script):
        """Runs `script`, then the built mock on the request, which is appended to requests.jsonl first."""
        executable(self.dir / "providers" / "mock",
                   f'#!/bin/sh\n{script}\nIFS= read -r request\nprintf "%s\\n" "$request" >> "${{0%/*}}/../requests.jsonl"\n'
                   f'printf "%s" "$request" | exec "{BUILD / "system" / "providers" / "mock"}"\n')

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


def _read_jsonl(path, skip_invalid=False):
    """Complete lines as JSON; skip_invalid drops lines that do not parse, as the kernel does in a
    session file."""
    if not path.exists():
        return []
    lines = [line for line in path.read_text().splitlines(keepends=True) if line.endswith("\n")]
    return [v for v in map(_parse_or_none if skip_invalid else json.loads, lines) if v is not None]


def _parse_or_none(line):
    try:
        return json.loads(line)
    except json.JSONDecodeError:
        return None


_HEADER = re.compile(r"\[([^\n]*)\]")
_ENDING = re.compile(r"(exit|signal) (\d+)")
_CUT = re.compile(r"cut at (\d+) of (\d+) bytes, (?:all of it in (.+)|saving all of it to (.+) failed: (.+))")
_HIDDEN = re.compile(r"(\d+) bytes of (.+) not shown, (?:all of it in (.+)|saving all of it to (.+) failed: (.+))")


def parse_shell(text):
    """A shell result's text as a dict: output, exit (or signal), and when they apply
    total and file (cut or not shown), file_error, and hidden (what was not shown)."""
    header, _, output = text.partition("\n")
    match = _HEADER.fullmatch(header)
    assert match, f"no header line: {text[:200]!r}"
    result = {"output": output}
    ending, *parts = match.group(1).split("; ")
    kind, n = _ENDING.fullmatch(ending).groups()
    result[kind] = int(n)
    for part in parts:
        if cut := _CUT.fullmatch(part):
            _, total, file, failed_file, error = cut.groups()
        elif hidden := _HIDDEN.fullmatch(part):
            total, result["hidden"], file, failed_file, error = hidden.groups()
        else:
            raise AssertionError(f"unknown header part: {part!r}")
        result["total"], result["file"] = int(total), file or failed_file
        if error:
            result["file_error"] = error
    return result


def _shell_or_text(text):
    return parse_shell(text) if _HEADER.fullmatch(text.partition("\n")[0]) else text


def _blocks(message):
    content = message["content"]
    return content if isinstance(content, list) else [{"type": "text", "text": content}]


def _text(message):
    return "".join(b["text"] for b in _blocks(message) if b["type"] == "text")
