#!/usr/bin/env python3
"""Size/day rotated launcher output. Python 3.6; never copy-truncate live files."""
from datetime import datetime
import os
from pathlib import Path
import re
import select
import sys
import time


def prune(root, role, days, current=None, now=None):
    cutoff = (time.time() if now is None else now) - days * 86400
    pattern = re.compile(re.escape(role) + r"-service(?:\.[0-9]+)?\.log\Z")
    for day in root.iterdir():
        if day.is_symlink() or not day.is_dir() or not re.fullmatch(r"[0-9]{8}", day.name):
            continue
        for path in day.iterdir():
            if (
                path != current
                and not path.is_symlink()
                and path.is_file()
                and pattern.fullmatch(path.name)
                and path.stat().st_mtime < cutoff
            ):
                # Do not unlink files still held by an older launcher/process.
                opened = False
                for process in Path("/proc").glob("[0-9]*"):
                    try:
                        if process.stat().st_uid != os.getuid():
                            continue
                        if any(fd.resolve() == path.resolve() for fd in (process / "fd").iterdir()):
                            opened = True
                            break
                    except (OSError, RuntimeError):
                        continue
                if not opened:
                    path.unlink()


def main():
    root, role, days, maximum = Path(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    if role not in ("server", "worker") or not 1 <= days <= 3650 or maximum < 65536:
        raise ValueError("invalid service log settings")
    stream = None
    path = None
    day_name = None
    next_prune = 0
    size = 0
    try:
        while True:
            if not select.select([0], [], [], 30)[0]:
                block = None
            else:
                block = os.read(0, 16384)
                if not block:
                    return
            today = datetime.now().strftime("%Y%m%d")
            if stream is None or today != day_name or size >= maximum:
                if stream is not None:
                    stream.close()
                day_name = today
                directory = root / today
                directory.mkdir(parents=True, exist_ok=True)
                # Unique files avoid concurrent/old sinks writing one target.
                index = time.time_ns() if hasattr(time, "time_ns") else int(time.time() * 1000000)
                path = directory / ("{}-service.{}.log".format(role, index))
                stream = path.open("xb", buffering=0)
                size = 0
            if time.monotonic() >= next_prune:
                prune(root, role, days, path)
                next_prune = time.monotonic() + 3600
            if block:
                stream.write(block)
                size += len(block)
    finally:
        if stream is not None:
            stream.close()


if __name__ == "__main__":
    main()
