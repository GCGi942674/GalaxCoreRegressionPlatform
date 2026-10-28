"""Stable local machine identity and safe reuse of legacy SVN slot copies."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import uuid

from recovery_journal import write_json

VALID_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}\Z")
IDENTITY_NAME = ".machine-identity.json"


def machine_identity(output_root):
    output_root = Path(output_root)
    output_root.mkdir(parents=True, exist_ok=True)
    lock_path = output_root / ".machine-identity.lock"
    fd = os.open(str(lock_path), os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        path = output_root / IDENTITY_NAME
        if path.is_symlink():
            raise RuntimeError("machine identity must not be a symlink: " + str(path))
        if path.exists():
            data = json.loads(path.read_text(encoding="utf-8"))
            if (
                not isinstance(data, dict)
                or data.get("version") != 1
                or not isinstance(data.get("name"), str)
                or not VALID_ID.fullmatch(data["name"])
                or not isinstance(data.get("uid"), str)
                or not re.fullmatch(r"[0-9a-f]{32}", data["uid"])
            ):
                raise RuntimeError("invalid machine identity: " + str(path))
            return data
        raw = os.environ.get("GCRP_MACHINE_NAME") or socket.gethostname().split(".")[0]
        name = re.sub(r"[^A-Za-z0-9_-]", "_", raw)[:56]
        if not name or not name[0].isalnum():
            name = "host-" + uuid.uuid4().hex[:12]
        data = dict(version=1, name=name, uid=uuid.uuid4().hex)
        write_json(path, data)
        return data
    finally:
        os.close(fd)


def slot_names(count):
    if type(count) is not int or not 1 <= count <= 128:
        raise ValueError("worker.slots must be 1..128")
    return ["slot{}".format(index) for index in range(1, count + 1)]


def migration_plan(slots_root, machine, slots):
    root = Path(slots_root)
    if not root.is_dir():
        return []
    plan = []
    for slot in slots:
        destination = root / (machine + "-" + slot)
        if destination.exists() or destination.is_symlink():
            continue
        index = int(slot[4:])
        pattern = re.compile(
            r"^(?P<old_machine>"
            + re.escape(machine)
            + r".*)-(?P<old_slot>slot[-_]?0*"
            + str(index)
            + r")$"
        )
        matches = []
        for candidate in root.iterdir():
            if not candidate.is_dir() or candidate.is_symlink():
                continue
            match = pattern.fullmatch(candidate.name)
            if (
                match
                and (candidate / "galaxcore/.svn").is_dir()
                and (candidate / "galaxcore/test2/run.sh").is_file()
            ):
                matches.append(
                    (candidate, destination, match.group("old_machine"), match.group("old_slot"))
                )
        if len(matches) > 1:
            raise RuntimeError(
                "multiple legacy SVN copies match {}: {}".format(
                    slot, ", ".join(str(item[0]) for item in matches)
                )
            )
        plan.extend(matches)
    return plan


def _try_lock(path):
    if not path.is_file():
        return None
    fd = os.open(str(path), os.O_RDWR | os.O_NOFOLLOW)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return fd
    except (OSError, BlockingIOError):
        os.close(fd)
        raise RuntimeError("legacy slot is still owned: " + str(path))


def migrate_legacy_slots(slots_root, output_root, machine, slots, preview=False):
    plan = migration_plan(slots_root, machine, slots)
    for source, target, _old_machine, _old_slot in plan:
        print("[MANAGER] legacy SVN reuse: {} -> {}".format(source, target), flush=True)
    if preview or not plan:
        return plan
    locks = []
    try:
        for source, target, old_machine, old_slot in plan:
            old_output = Path(output_root) / old_machine / old_slot
            if old_output.is_dir():
                if list(old_output.glob("pending-report-*.json")) or list(
                    old_output.glob("active-*.json")
                ):
                    raise RuntimeError("legacy slot has unresolved task state: " + str(old_output))
                old_id = source.name
                identity = hashlib.sha256(old_id.encode("utf-8")).hexdigest()[:24]
                for lock_path in (
                    Path(output_root) / old_machine / ".manager.lock",
                    old_output / ("active-" + identity + ".json.lock"),
                ):
                    fd = _try_lock(lock_path)
                    if fd is not None:
                        locks.append(fd)
            if target.exists() or target.is_symlink() or not source.is_dir():
                raise RuntimeError("SVN migration target changed: " + str(target))
        for source, target, _old_machine, _old_slot in plan:
            source.rename(target)
            print("[MANAGER] reused SVN copy: {}".format(target), flush=True)
        return plan
    finally:
        for fd in locks:
            os.close(fd)
