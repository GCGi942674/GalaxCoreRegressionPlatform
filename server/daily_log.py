"""Small per-invocation Clock log with conservative 30-day cleanup (Python 3.6)."""
from contextlib import contextmanager, redirect_stdout, redirect_stderr
from datetime import datetime
import os
from pathlib import Path
import re
import sys
import time
import uuid

DAY = re.compile(r"[0-9]{8}\Z")
NAME = re.compile(r"clock-[0-9]{6}-[0-9]+-[0-9a-f]{8}\.log\Z")


class Tee:
    def __init__(self, console, logfile):
        self.console, self.logfile = console, logfile

    def write(self, text):
        self.logfile.write(text)
        self.logfile.flush()
        return self.console.write(text)

    def flush(self):
        self.console.flush()
        self.logfile.flush()


def prune(root, days, now=None):
    cutoff = (time.time() if now is None else now) - days * 86400
    root = Path(root)
    if root.is_symlink() or not root.is_dir():
        raise ValueError("unsafe daily log root")
    for day in root.iterdir():
        if day.is_symlink() or not day.is_dir() or not DAY.fullmatch(day.name):
            continue
        for file in day.iterdir():
            if (
                not file.is_symlink()
                and file.is_file()
                and NAME.fullmatch(file.name)
                and file.stat().st_mtime < cutoff
            ):
                file.unlink()
        # Leave unknown files and directory metadata alone.


@contextmanager
def daily_log(config):
    root = Path(config.get("server.daily_log_root") or "./logs/daily").expanduser()
    days = int(config.get("log.retention_days", "30"))
    if not 1 <= days <= 3650:
        raise ValueError("invalid log retention days")
    if root.is_symlink():
        raise ValueError("unsafe daily log root")
    root.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now()
    day = root / stamp.strftime("%Y%m%d")
    if day.is_symlink():
        raise ValueError("unsafe daily log day")
    day.mkdir(exist_ok=True)
    path = day / (
        "clock-{}-{}-{}.log".format(stamp.strftime("%H%M%S"), os.getpid(), uuid.uuid4().hex[:8])
    )
    with path.open("x", encoding="utf-8") as stream:
        with redirect_stdout(Tee(sys.stdout, stream)), redirect_stderr(Tee(sys.stderr, stream)):
            print("[CLOCK] log=" + str(path), flush=True)
            try:
                yield path
            finally:
                print("[CLOCK] invocation finished", flush=True)
    prune(root, days)
