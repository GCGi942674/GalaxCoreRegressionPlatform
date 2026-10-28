"""Validated Linux process identity for Manager crash handover; Python 3.6+."""
import json
import os
from pathlib import Path
from recovery_journal import write_json


def start_ticks(pid):
    try:
        fields = Path("/proc/{}/stat".format(pid)).read_text().rsplit(")", 1)[1].split()
        return None if fields[0] == "Z" else fields[19]
    except (OSError, IndexError):
        return None


def publish(output, worker_id, script):
    write_json(
        Path(output) / ".worker-process.json",
        dict(
            version=1,
            pid=os.getpid(),
            start_ticks=start_ticks(os.getpid()),
            worker_id=worker_id,
            output=str(Path(output).resolve()),
            script=str(Path(script).resolve()),
        ),
    )


class ExistingWorker:
    def __init__(self, output, worker_id, script, locked):
        self.output = Path(output).resolve()
        self.worker_id = worker_id
        self.script = str(Path(script).resolve())
        path = self.output / ".worker-process.json"
        if path.is_symlink():
            raise RuntimeError("unsafe Worker process record: " + str(path))
        try:
            self.record = json.loads(path.read_text())
            self.pid = self.record["pid"]
            if type(self.pid) is not int or self.pid <= 1 or not self.matches():
                raise ValueError("identity mismatch")
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise RuntimeError(
                "existing slot cannot be safely adopted; inspect its PID/journal: " + str(path)
            ) from error
        self.locked = locked

    def matches(self):
        r = self.record
        if (
            r.get("version") != 1
            or r.get("worker_id") != self.worker_id
            or r.get("script") != self.script
            or r.get("output") != str(self.output)
            or not r.get("start_ticks")
            or start_ticks(self.pid) != r["start_ticks"]
        ):
            return False
        try:
            args = Path("/proc/{}/cmdline".format(self.pid)).read_bytes().split(b"\0")
            args = [arg.decode("utf-8") for arg in args if arg]
            return (
                self.script in args
                and args[args.index("--worker-id") + 1] == self.worker_id
                and Path(args[args.index("--output") + 1]).resolve() == self.output
            )
        except (OSError, ValueError, IndexError):
            return False

    def poll(self):
        if self.matches():
            return None
        if self.locked():
            raise RuntimeError(
                "adopted slot identity changed while lock remains held; inspect " + str(self.output)
            )
        return 0
