"""Linux subreaper owning one execution, including detached descendants.

The lease pipe closes on Worker death. Never kill this supervisor to impose a
timeout: it withholds completion until every owned descendant has been reaped.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import time
from recovery_journal import write_json


def descendants():
    parents = {}
    for item in Path("/proc").iterdir():
        if not item.name.isdigit():
            continue
        try:
            fields = (item / "stat").read_text().rsplit(")", 1)[1].split()
            parents[int(item.name)] = int(fields[1])
        except (OSError, ValueError, IndexError):
            continue
    owned = {os.getpid()}
    while True:
        more = {pid for pid, parent in parents.items() if parent in owned}
        if more <= owned:
            return owned - {os.getpid()}
        owned.update(more)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lease", type=int, required=True)
    parser.add_argument("--completion", type=int, required=True)
    parser.add_argument("--timeout", type=float, required=True)
    parser.add_argument("--grace", type=float, required=True)
    parser.add_argument("--receipt")
    parser.add_argument("--events-stderr", action="store_true")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), "cannot establish execution subreaper")
    stopping = [False]
    signal.signal(signal.SIGTERM, lambda *_: stopping.__setitem__(0, True))
    signal.signal(signal.SIGINT, lambda *_: stopping.__setitem__(0, True))
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    child = subprocess.Popen(command, start_new_session=True, close_fds=True)
    command_started = time.monotonic()
    try:
        print(
            json.dumps(
                dict(
                    event="execution_started",
                    supervisor_pid=os.getpid(),
                    command_pid=child.pid,
                    argv=command,
                )
            ),
            file=sys.stderr if args.events_stderr else sys.stdout,
            flush=True,
        )
    except OSError:
        stopping[0] = True
    deadline = command_started + args.timeout
    code = None
    timed_out = False
    stop_reason = None
    while code is None:
        code = child.poll()
        expired = time.monotonic() >= deadline
        lost = bool(select.select([args.lease], [], [], 0)[0])
        if expired or lost or stopping[0]:
            timed_out = expired
            stop_reason = "timeout" if expired else "lease_lost" if lost else "signal"
            break
        time.sleep(0.02)
    command_finished = time.monotonic()
    leaked = code is not None and bool(descendants())
    kill_at = time.monotonic() + args.grace
    while True:
        owned = descendants()
        if not owned:
            # /proc traversal is not an atomic snapshot. ECHILD, not an empty
            # traversal, is the authoritative proof that no descendant remains.
            child.poll()
            if child.returncode is not None:
                try:
                    os.waitpid(-1, os.WNOHANG)
                except ChildProcessError:
                    break
            time.sleep(0.02)
            continue
        sig = signal.SIGKILL if time.monotonic() >= kill_at else signal.SIGTERM
        for pid in owned:
            try:
                os.kill(pid, sig)
            except ProcessLookupError:
                pass
        child.poll()
        if child.returncode is not None:
            while True:
                try:
                    if os.waitpid(-1, os.WNOHANG)[0] == 0:
                        break
                except ChildProcessError:
                    break
        time.sleep(0.02)
    if args.receipt:
        write_json(args.receipt, dict(clean=True, token=Path(args.receipt).name))
    try:
        # Preserve the leading proof byte for older Workers. The bounded tail
        # distinguishes a drained descendant from a deadline or lost lease.
        completion = dict(
            reason=stop_reason or ("descendants_reaped" if leaked else "command_exit"),
            command_exit_code=code,
            command_wall_ms=max(0, int((command_finished - command_started) * 1000)),
            reap_wall_ms=max(0, int((time.monotonic() - command_finished) * 1000)),
        )
        os.write(args.completion, b"C" + json.dumps(completion).encode("ascii"))
    except BrokenPipeError:
        pass  # Worker disappeared; descendants are still fully reaped.
    if timed_out:
        return 124
    if stopping[0] or code is None or leaked:
        return 125
    return code if code >= 0 else 128 - code


if __name__ == "__main__":
    raise SystemExit(main())
