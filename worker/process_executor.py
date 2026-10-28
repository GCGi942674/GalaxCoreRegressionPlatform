"""Run commands under a dedicated Linux descendant supervisor."""
import os
import json
from pathlib import Path
import subprocess
import sys
import time


class ExecutionContainmentError(RuntimeError):
    """Supervisor exited without proving that the execution tree is empty."""


class ProcessExecutor:
    def __init__(self, heartbeat=None):
        self.process = None
        self.lease = None
        self.completion = None
        self.safe = True
        self.last_completion = {}
        self.heartbeat = heartbeat or (lambda: None)
        self.before_launch = None
        self.events_stderr = False

    def stop(self, grace=0):
        if self.lease is not None:
            os.close(self.lease)
            self.lease = None
        if self.process is not None:
            # EOF is safe even before the guard installs signal handlers.
            # A direct SIGTERM here could kill a just-starting supervisor.
            self.process.wait()
            self.process = None
        if self.completion is not None:
            try:
                self.safe = os.read(self.completion, 1) == b"C"
                if self.safe:
                    details = os.read(self.completion, 4096)
                    if details:
                        try:
                            report = json.loads(details.decode("ascii"))
                            if isinstance(report, dict):
                                self.last_completion = report
                        except (ValueError, UnicodeError):
                            # An unknown diagnostics tail never substitutes for
                            # the leading clean-tree proof or a case verdict.
                            self.last_completion = {}
            finally:
                os.close(self.completion)
                self.completion = None
            if not self.safe:
                raise ExecutionContainmentError("execution supervisor lost; quarantine workspace")

    def run(self, argv, cwd, env, stdout, stderr, timeout, grace=2, on_start=None):
        if not self.safe:
            raise ExecutionContainmentError(
                "execution supervisor lost; restart requires inspection"
            )
        self.last_completion = {}
        receipt = self.before_launch() if self.before_launch else None
        read_fd, self.lease = os.pipe()
        self.completion, completion_write = os.pipe()
        try:
            self.process = subprocess.Popen(
                [
                    sys.executable,
                    "-B",
                    str(Path(__file__).with_name("execution_guard.py")),
                    "--lease",
                    str(read_fd),
                    "--timeout",
                    str(timeout),
                    "--grace",
                    str(grace),
                    "--completion",
                    str(completion_write),
                    *(["--receipt", str(receipt)] if receipt else []),
                    *(["--events-stderr"] if self.events_stderr else []),
                    "--",
                    *map(str, argv),
                ],
                cwd=str(cwd),
                env=env,
                stdout=stdout,
                stderr=stderr,
                start_new_session=True,
                pass_fds=(read_fd, completion_write),
            )
        except BaseException:
            os.close(self.completion)
            self.completion = None
            self.stop(grace)
            raise
        finally:
            os.close(read_fd)
            os.close(completion_write)
        pid = self.process.pid
        try:
            if on_start:
                on_start(pid)
            next_heartbeat = time.monotonic()
            while self.process.poll() is None:
                if time.monotonic() >= next_heartbeat:
                    self.heartbeat()
                    next_heartbeat = time.monotonic() + 1
                time.sleep(0.02)
            code = self.process.wait()
            return pid, code, code == 124
        finally:
            self.stop(grace)
