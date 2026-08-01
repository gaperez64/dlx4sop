import pathlib
import re
import subprocess


def assert_version_output(exe: pathlib.Path, program: str) -> None:
    completed = subprocess.run(
        [str(exe), "--version"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    pattern = rf"{re.escape(program)} (\d[\w.+-]*|[0-9a-f]{{7,40}})(-dirty)?\n"
    if completed.returncode != 0 or re.fullmatch(pattern, completed.stdout) is None:
        raise AssertionError(
            f"unexpected --version result:\n{completed.stdout}\n{completed.stderr}"
        )
