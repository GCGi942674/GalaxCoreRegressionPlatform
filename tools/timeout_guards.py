#!/usr/bin/env python3
"""List timeout protection; clearing requires a stopped Server and an explicit key."""
import argparse
from contextlib import closing
from datetime import datetime
import fcntl
import json
import os
from pathlib import Path
import sqlite3
import sys
import uuid

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / "worker"))
from runtime_config import load_config
from backup_database import _restore_dump


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config")
    parser.add_argument("--clear", metavar="GUARD_KEY")
    parser.add_argument("--confirm", action="store_true")
    args = parser.parse_args()
    if args.config:
        os.environ["GCRP_CONFIG"] = str(Path(args.config).expanduser().resolve())
    os.chdir(str(PROJECT))
    lock = None
    try:
        config = load_config("server")
        db = Path(config["database.path"]).expanduser().resolve(strict=True)
        if not args.clear:
            with closing(sqlite3.connect(db.as_uri() + "?mode=ro", uri=True)) as connection:
                rows = connection.execute(
                    "SELECT guard_key,run_id,created_at_ms FROM timeout_guards ORDER BY created_at_ms DESC"
                ).fetchall()
                print(
                    json.dumps(
                        [dict(key=r[0], run_id=r[1], created_at_ms=r[2]) for r in rows], indent=2
                    )
                )
            return 0
        if not args.confirm:
            raise ValueError("--clear requires --confirm after inspecting the triggering run")
        lock = os.open(str(db) + ".lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            raise ValueError("stop Server before clearing timeout protection")
        root = Path(config.get("database.backup_root") or str(db.parent / "backups")).expanduser()
        root.mkdir(parents=True, exist_ok=True)
        target = root / (
            "gcrp-timeout-reset-"
            + datetime.utcnow().strftime("%Y%m%dT%H%M%SZ")
            + "-"
            + uuid.uuid4().hex[:8]
            + ".db"
        )
        with closing(sqlite3.connect(db.as_uri() + "?mode=ro", uri=True)) as original:
            if not original.execute(
                "SELECT 1 FROM timeout_guards WHERE guard_key=?", (args.clear,)
            ).fetchone():
                raise ValueError("guard key not found")
            with closing(sqlite3.connect(str(target))) as backup:
                if hasattr(original, "backup"):
                    original.backup(backup)
                else:
                    original.execute("BEGIN")
                    _restore_dump(backup, original.iterdump())
                    original.rollback()
                if backup.execute("PRAGMA quick_check").fetchone()[0] != "ok":
                    raise ValueError("backup validation failed")
        with closing(sqlite3.connect(str(db))) as connection:
            with connection:
                cursor = connection.execute(
                    "DELETE FROM timeout_guards WHERE guard_key=?", (args.clear,)
                )
                if cursor.rowcount != 1:
                    raise ValueError("guard changed; inspect before retry")
        print("CLEARED: " + args.clear + "\nBACKUP: " + str(target))
        return 0
    except (OSError, ValueError, sqlite3.Error, KeyError) as error:
        print("[TIMEOUT-GUARD] " + str(error), file=sys.stderr)
        return 1
    finally:
        if lock is not None:
            os.close(lock)


if __name__ == "__main__":
    sys.exit(main())
