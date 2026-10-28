"""Small, versioned timing and timeout helpers (Python 3.6 compatible)."""
import hashlib
import json
from pathlib import Path

SCOPE = "single_case_runner_wall_v1"


def script_fingerprint(root, case):
    root, case = Path(root), Path(case)
    files = [root / "run.sh", root / "run_proc.tcl", case]
    lib = root / "vivado_runner" / "lib"
    if lib.is_dir():
        files.extend(p for p in lib.rglob("*") if p.is_file() and p.suffix in (".sh", ".py"))
    digest = hashlib.sha256()
    total = 0
    for path in sorted(set(files), key=lambda p: str(p.relative_to(root))):
        if (
            path.is_symlink()
            or not path.is_file()
            or any(p.is_symlink() for p in path.parents if p != root and root in p.parents)
        ):
            raise ValueError("unsafe timing fingerprint input")
        size = path.stat().st_size
        total += size
        if total > 16 * 1024 * 1024:
            raise ValueError("timing fingerprint inputs too large")
        digest.update(str(path.relative_to(root)).encode("utf-8") + b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def select_timeout(task, fingerprint, archive_sha256):
    requested = task["payload"]["timeout_seconds"]
    actual, reason = requested, "fixed"
    decision = task.get("timeout_decision")
    if isinstance(decision, dict):
        reason = "observe" if decision.get("mode") == "observe" else "fixed"
        if decision.get("applied") is True:
            valid = (
                task.get("_timeout_v1") is True
                and decision.get("version") == 1
                and decision.get("mode") == "enforce"
                and decision.get("decision_id") == task.get("run_id")
                and decision.get("requested_seconds") == requested
                and type(decision.get("effective_seconds")) is int
                and 1 <= decision["effective_seconds"] <= requested
            )
            if not valid:
                reason = "invalid_decision"
            elif decision.get("expected_fingerprint") != fingerprint:
                reason = "script_fingerprint_changed"
            elif decision.get("expected_archive_sha256") != archive_sha256:
                reason = "archive_fingerprint_changed"
            else:
                actual, reason = decision["effective_seconds"], "auto_applied"
    return actual, reason


def valid_timing(value):
    if not isinstance(value, dict) or value.get("version") != 1 or value.get("scope") != SCOPE:
        return False
    for key in ("prepare_wall_ms", "command_wall_ms", "reap_wall_ms", "finalize_wall_ms"):
        if type(value.get(key)) is not int or not 0 <= value[key] <= 7 * 86400000:
            return False
    return (
        type(value.get("effective_timeout_seconds")) is int
        and 1 <= value["effective_timeout_seconds"] <= 86400
        and all(
            isinstance(value.get(k), str)
            and len(value[k]) == 64
            and all(c in "0123456789abcdef" for c in value[k])
            for k in ("script_fingerprint", "archive_sha256")
        )
    )
