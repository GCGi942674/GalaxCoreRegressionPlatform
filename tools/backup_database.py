#!/usr/bin/env python3
"""Consistent SQLite backup, including uncheckpointed WAL; Python 3.6+."""
import argparse
import fcntl
from contextlib import closing
from datetime import date
import os
import re
from pathlib import Path
import shutil
import sqlite3
import sys
import tempfile
import time


def _restore_dump(copy, statements):
    """Replay old CPython dumps without restoring AUTOINCREMENT state too early."""
    sequences = []
    sequence_write = re.compile(
        r'^\s*(?:DELETE\s+FROM|INSERT\s+INTO)\s+"?sqlite_sequence"?(?:\s|;)', re.I
    )
    for statement in statements:
        if sequence_write.match(statement):
            sequences.append(statement)
            continue
        if statement.strip().rstrip(";").upper() == "COMMIT":
            # Python 3.6 emits sqlite_sequence before tables sorted after 'sqlite_'.
            # Those CREATE TABLE ... AUTOINCREMENT statements must run first.
            exists = copy.execute(
                "SELECT 1 FROM sqlite_master WHERE type='table' AND name='sqlite_sequence'"
            ).fetchone()
            if exists:
                for sequence in sequences:
                    copy.execute(sequence)
            elif any(item.lstrip().upper().startswith("INSERT") for item in sequences):
                raise RuntimeError("cannot restore AUTOINCREMENT state without sqlite_sequence")
            sequences = []
        copy.execute(statement)
    if sequences:
        raise RuntimeError("incomplete SQLite dump: missing COMMIT")


def _backup_database(config, logical=False):
    source = Path(config["database.path"]).expanduser().resolve(strict=True)
    root = Path(config.get("database.backup_root") or str(source.parent / "backups")).expanduser()
    if root.is_symlink():
        raise ValueError("backup root must not be a symlink")
    root.mkdir(parents=True, exist_ok=True)
    minimum = int(config.get("database.min_free_bytes", "536870912"))
    if minimum < 0 or shutil.disk_usage(str(root)).free < max(minimum, source.stat().st_size * 2):
        raise RuntimeError("insufficient free space for database backup")
    target = root / ("gcrp-backup-" + date.today().strftime("%Y%m%d") + ".db")
    days = int(config.get("database.backup_days", "7"))
    if not 1 <= days <= 3650:
        raise ValueError("invalid backup_days")
    if target.is_symlink():
        raise ValueError("unsafe backup target")
    if target.exists():
        with closing(sqlite3.connect(target.resolve().as_uri() + "?mode=ro", uri=True)) as check:
            if check.execute("PRAGMA quick_check").fetchone()[0] != "ok":
                raise RuntimeError("existing backup is corrupt; inspect " + str(target))
        return target
    descriptor, name = tempfile.mkstemp(prefix=".backup-", dir=str(root))
    os.close(descriptor)
    temporary = Path(name)
    try:
        with closing(
            sqlite3.connect(source.as_uri() + "?mode=ro", uri=True, timeout=10)
        ) as original:
            with closing(sqlite3.connect(str(temporary))) as copy:
                if hasattr(original, "backup") and not logical:
                    original.backup(copy, pages=128, sleep=0.05)
                else:
                    # Python 3.6 lacks Connection.backup. Hold one read snapshot
                    # while reconstructing a private database, not a raw file copy.
                    original.execute("BEGIN")
                    _restore_dump(copy, original.iterdump())
                    version = original.execute("PRAGMA user_version").fetchone()[0]
                    copy.execute("PRAGMA user_version={}".format(int(version)))
                    copy.commit()
                if copy.execute("PRAGMA quick_check").fetchone()[0] != "ok":
                    raise RuntimeError("backup integrity check failed")
                if copy.execute("PRAGMA foreign_key_check").fetchone() is not None:
                    raise RuntimeError("backup foreign key check failed")
        with temporary.open("rb") as stream:
            os.fsync(stream.fileno())
        os.replace(str(temporary), str(target))
        directory = os.open(str(root), os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
        cutoff = time.time() - days * 86400
        for path in root.glob("gcrp-backup-????????.db"):
            if not re.fullmatch(r"gcrp-backup-[0-9]{8}\.db", path.name):
                continue
            if (
                path != target
                and not path.is_symlink()
                and path.is_file()
                and path.stat().st_mtime < cutoff
            ):
                path.unlink()
        return target
    finally:
        if temporary.exists():
            temporary.unlink()


def backup_database(config, logical=False):
    source = Path(config["database.path"]).expanduser().resolve(strict=True)
    root = Path(config.get("database.backup_root") or str(source.parent / "backups")).expanduser()
    if root.is_symlink():
        raise ValueError("unsafe backup root")
    root.mkdir(parents=True, exist_ok=True)
    fd = os.open(str(root / ".backup.lock"), os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        return _backup_database(config, logical)
    finally:
        os.close(fd)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "worker"))
    from runtime_config import load_config

    try:
        print("[BACKUP] " + str(backup_database(load_config("server"))))
    except (OSError, ValueError, RuntimeError, sqlite3.Error) as error:
        print("[BACKUP] FAIL: " + str(error), file=sys.stderr)
        sys.exit(1)
