"""The shipped system-prompt program: the base prompt, the user's system.md, the skills list and the project's context files."""

import datetime
import os
import subprocess

from conftest import BUILD, Kernel

BASE = (BUILD / "system" / "base-prompt.md").read_text()


def here(path, repo=None):
    """The environment lines: working directory, repository, date and system."""
    lines = f"Your working directory is {path}.\n"
    if repo:
        lines += f"It is in the git repository {repo}.\n"
    uname = os.uname()
    return lines + f"Today's date is {datetime.date.today():%Y-%m-%d}.\nThe system is {uname.sysname} {uname.release}.\n"


def description(skill):
    return next(line.removeprefix("description: ") for line in skill.read_text().splitlines()
                if line.startswith("description: "))


def test_user_skills_come_first_and_the_header_ends_at_the_second_rule(kernel, home):
    mine = home.dir / "skills" / "zz" / "SKILL.md"
    mine.parent.mkdir(parents=True)
    mine.write_text("---\nname: my-zz\ndescription: mine\n---\n\nname: not the header\n")
    kernel("hi", env=home.env)
    akern = home.dir / "system" / "skills" / "akern" / "SKILL.md"
    assert home.requests[0]["system"] == (BASE + "\n" + here(kernel.dir) + "\nSkills: read a skill's file before relying on it.\n"
                                          f"- my-zz: mine ({mine})\n"
                                          f"- akern: {description(akern)} ({akern})\n")


def test_a_user_skill_hides_the_built_in_of_the_same_name(kernel, home):
    mine = home.dir / "skills" / "akern" / "SKILL.md"
    mine.parent.mkdir(parents=True)
    mine.write_text("---\nname: my-akern\ndescription: mine\n---\n")
    kernel("hi", env=home.env)
    assert home.requests[0]["system"] == (BASE + "\n" + here(kernel.dir) +
                                          f"\nSkills: read a skill's file before relying on it.\n- my-akern: mine ({mine})\n")


def test_system_md_then_skills_then_context_files_from_the_repository_root_down_to_the_working_directory(
        tmp_path, akern_env, home):
    repo = tmp_path / "repo"
    cwd = repo / "a" / "b"
    cwd.mkdir(parents=True)
    (repo / ".git").mkdir()
    (tmp_path / "AGENTS.md").write_text("outside the repository\n")
    (repo / "AGENTS.md").write_text("root")
    (repo / "a" / "CLAUDE.md").write_text("middle\n")
    (cwd / "AGENTS.md").write_text("here\n")
    (cwd / "CLAUDE.md").write_text("hidden by AGENTS.md\n")
    (home.dir / "system.md").write_text("mine\n")
    Kernel(cwd, akern_env)("hi", env=home.env)
    system = home.requests[0]["system"]
    assert system.startswith(BASE + "\n" + here(cwd, repo) + "\nmine\n\nSkills: read a skill's file before relying on it.\n- akern: ")
    assert system.endswith(f")\n\n# {repo}/AGENTS.md\n\nroot\n\n# {repo}/a/CLAUDE.md\n\nmiddle\n\n"
                           f"# {cwd}/AGENTS.md\n\nhere\n")
    assert "outside" not in system and "hidden" not in system


def test_outside_a_repository_only_the_working_directory_has_context_files(kernel, home, tmp_path):
    (tmp_path / "CLAUDE.md").write_text("here\n")
    kernel("hi", env=home.env)
    assert home.requests[0]["system"].endswith(f")\n\n# {tmp_path}/CLAUDE.md\n\nhere\n")


def test_your_base_prompt_replaces_the_shipped_one_and_an_empty_one_removes_it(kernel, home):
    (home.dir / "base-prompt.md").write_text("my base")
    kernel("hi", env=home.env)
    assert home.requests[0]["system"].startswith("my base\n\n" + here(kernel.dir) + "\nSkills: ")
    (home.dir / "base-prompt.md").write_text("")
    kernel("again", env=home.env)
    assert home.requests[1]["system"].startswith(here(kernel.dir) + "\nSkills: ")


def test_the_shipped_program_run_by_hand_renders_the_home_it_is_in(home, tmp_path):
    (home.dir / "system.md").write_text("mine\n")
    env = {k: v for k, v in os.environ.items() if k != "AKERN_HOME"} | {"AKERN_HOME": str(tmp_path / "elsewhere")}
    out = subprocess.run([str(home.dir / "system" / "system-prompt")], cwd=tmp_path, env=env,
                         capture_output=True, text=True, check=True).stdout
    assert out.startswith(BASE + "\n" + here(tmp_path) + "\nmine\n\nSkills: ")
