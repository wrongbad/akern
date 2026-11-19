"""The shipped prompt program: the user's system.md, the skills list and the project's context files."""

from conftest import Kernel


def description(skill):
    return next(line.removeprefix("description: ") for line in skill.read_text().splitlines()
                if line.startswith("description: "))


def test_a_user_skill_hides_the_built_in_of_the_same_name_and_user_skills_come_first(kernel, home):
    mine = home.dir / "skills" / "files" / "SKILL.md"
    mine.parent.mkdir(parents=True)
    mine.write_text("---\nname: my-files\ndescription: mine\n---\n\nname: not the header\n")
    kernel("hi", env=home.env)
    akern = home.dir / "system" / "skills" / "akern" / "SKILL.md"
    assert home.requests[0]["system"] == ("Skills: read a skill's file before relying on it.\n"
                                          f"- my-files: mine ({mine})\n"
                                          f"- akern: {description(akern)} ({akern})\n")


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
    assert system.startswith("mine\n\nSkills: read a skill's file before relying on it.\n- akern: ")
    assert system.endswith(f")\n\n# {repo}/AGENTS.md\n\nroot\n\n# {repo}/a/CLAUDE.md\n\nmiddle\n\n"
                           f"# {cwd}/AGENTS.md\n\nhere\n")
    assert "outside" not in system and "hidden" not in system


def test_outside_a_repository_only_the_working_directory_has_context_files(kernel, home, tmp_path):
    (tmp_path / "CLAUDE.md").write_text("here\n")
    kernel("hi", env=home.env)
    assert home.requests[0]["system"].endswith(f")\n\n# {tmp_path}/CLAUDE.md\n\nhere\n")
