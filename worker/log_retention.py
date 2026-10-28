"""Conservative cleanup of completed Worker execution output (Python 3.6 compatible)."""
import fcntl
import json
import os
from pathlib import Path
from recovery_journal import sync_directory
import re
import shutil
import stat
import time

_RUN = re.compile(r"run-[A-Za-z0-9_]+\Z")
_TASK = re.compile(r"[1-9][0-9]*\Z")


def _protected(output):
    ids = set()
    directories = set()
    for pattern in ("active-*.json", "pending-report-*.json", "pending-logs-*.json"):
        for path in output.glob(pattern):
            if path.is_symlink():
                raise RuntimeError("unsafe Worker journal: " + str(path))
            data = json.loads(path.read_text(encoding="utf-8"))
            task_id = data.get("task_id")
            if type(task_id) is not int or task_id < 1:
                raise RuntimeError("invalid Worker journal: " + str(path))
            ids.add(str(task_id))
            if pattern == "pending-logs-*.json":
                directory = data.get("directory")
                if not isinstance(directory, str):
                    raise RuntimeError("invalid log outbox: " + str(path))
                directories.add(directory)
    return ids, directories


def _newest_and_safe(path):
    """Do not follow links or delete a tree containing unexpected link targets."""
    newest = path.stat().st_mtime
    for current, dirs, files in os.walk(str(path), followlinks=False):
        for name in dirs + files:
            child = Path(current) / name
            if child.is_symlink():
                return None
            newest = max(newest, child.stat().st_mtime)
    return newest


def remove_task_artifact(directory):
    """Remove only the private input ZIP, never the shared source or a symlink."""
    directory = Path(directory)
    if any(path.is_symlink() for path in (directory,) + tuple(directory.parents)):
        raise RuntimeError("unsafe artifact directory")
    archive = directory / "artifact.zip"
    try:
        info = archive.lstat()
    except FileNotFoundError:
        return 0
    if not stat.S_ISREG(info.st_mode):
        raise RuntimeError("unsafe task artifact")
    archive.unlink()
    sync_directory(directory)
    return info.st_size


def _archived_artifact(directory):
    receipt = directory / "archived.json"
    if receipt.is_symlink() or not receipt.is_file():
        return 0
    value = json.loads(receipt.read_text(encoding="utf-8"))
    if (
        not isinstance(value, dict)
        or value.get("server_verified") is not True
        or not isinstance(value.get("verified_files"), dict)
        or not value["verified_files"]
    ):
        return 0
    return remove_task_artifact(directory)


def prune_worker_logs(output, days=30, now=None):
    if type(days) is not int or not 1 <= days <= 3650:
        raise ValueError("invalid log retention days")
    output = Path(output)
    if output.is_symlink() or not output.is_dir():
        raise RuntimeError("unsafe Worker output root")
    cutoff = (time.time() if now is None else now) - days * 86400
    lock_path = output / ".retention.lock"
    fd = os.open(str(lock_path), os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    removed = 0
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        protected_ids, protected_directories = _protected(output)
        for run in output.iterdir():
            if run.is_symlink() or not run.is_dir() or not _RUN.fullmatch(run.name):
                continue
            for task in run.iterdir():
                if (
                    task.is_symlink()
                    or not task.is_dir()
                    or not _TASK.fullmatch(task.name)
                    or task.name in protected_ids
                    or str(task.resolve()) in protected_directories
                ):
                    continue
                # Completed, Server-verified runs no longer need the input ZIP.
                # Keep journals protected above; do not wait 30 days for this copy.
                reclaimed = _archived_artifact(task)
                if reclaimed:
                    print("[WORKER] removed archived task artifact: {} ({} bytes)".format(
                        task, reclaimed), flush=True)
                newest = _newest_and_safe(task)
                if newest is None or newest >= cutoff:
                    continue
                # A journal could appear while we scan. All Worker slots use this output root.
                if task.name in _protected(output)[0]:
                    continue
                shutil.rmtree(str(task))
                removed += 1
            if run.stat().st_mtime < cutoff and not any(run.iterdir()):
                run.rmdir()
        # Completion receipts outside run directories used to accumulate forever.
        # Keep referenced receipts and a one-day safety margin for late guards.
        referenced = set()
        for pattern in ("active-*.json", "pending-report-*.json"):
            for journal in output.glob(pattern):
                if journal.is_symlink():
                    raise RuntimeError("unsafe receipt journal")
                value = json.loads(journal.read_text(encoding="utf-8"))
                if isinstance(value.get("receipt"), str):
                    referenced.add(str(Path(value["receipt"]).resolve()))
        receipt_cutoff = (time.time() if now is None else now) - 86400
        for receipt in output.glob("clean-*.json"):
            if (
                receipt.is_symlink()
                or not receipt.is_file()
                or not re.fullmatch(r"clean-[0-9a-f]{32}\.json", receipt.name)
                or receipt.stat().st_mtime >= receipt_cutoff
                or str(receipt.resolve()) in referenced
            ):
                continue
            value = json.loads(receipt.read_text(encoding="utf-8"))
            if value == dict(clean=True, token=receipt.name):
                receipt.unlink()
                sync_directory(output)
    finally:
        os.close(fd)
    return removed
