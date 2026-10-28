#!/usr/bin/env python3
"""Development-only formatter; production Server/Worker do not need its tools.

Use clang-format 16 and Black 23.12.1 (run Black with development Python 3.8+).
Generated/runtime files and third-party sources are intentionally excluded.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="report formatting differences only")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    clang_format = shutil.which("clang-format")
    if not clang_format:
        parser.error("install clang-format 16 to format C++ sources")
    cpp = []
    python = []
    for folder in ("client", "common", "server", "gui/qt", "worker", "tools"):
        for directory, directories, files in os.walk(str(root / folder)):
            directories[:] = sorted(
                name
                for name in directories
                if name not in ("build", "__pycache__", "workers_slots", ".svn", ".git")
            )
            for name in sorted(files):
                path = Path(directory) / name
                if path.is_symlink():
                    continue
                relative = str(path.relative_to(root))
                if path.suffix in (".cpp", ".h", ".hpp"):
                    cpp.append(relative)
                elif path.suffix == ".py":
                    python.append(relative)
    if cpp:
        command = [clang_format, "--style=file"]
        command.extend(["--dry-run", "--Werror"] if args.check else ["-i"])
        subprocess.check_call(command + cpp, cwd=str(root))
    if python:
        command = [sys.executable, "-m", "black", "--config", "pyproject.toml"]
        if args.check:
            command.append("--check")
        subprocess.check_call(command + python, cwd=str(root))
    print(
        "Formatting {}: {} C++ files, {} Python files".format(
            "checked" if args.check else "updated", len(cpp), len(python)
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
