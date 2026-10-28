#!/usr/bin/env python3
"""Read-only storage capacity checks for cron/operations (Python 3.6+)."""
import argparse
import json
from pathlib import Path
import shutil
import sys


def storage_health(config, role):
    if role == "server":
        database = config.get("database.path")
        if not database:
            raise ValueError("configure database.path")
        paths = {
            "database": Path(database).parent,
            "archive": Path(config.get("server.log_archive_root") or "./data/run-logs"),
            "logs": Path(config.get("log.dir") or "./logs"),
        }
    else:
        paths = {
            "output": Path(config.get("worker.output_root") or "./worker-output"),
            "slots": Path(config.get("worker.slots_root") or "./worker/workers_slots"),
        }
    minimum = int(config.get("health.min_free_bytes", "536870912"))
    percent = float(config.get("health.min_free_percent", "10"))
    if minimum < 0 or not 0 <= percent <= 100:
        raise ValueError("invalid health free-space thresholds")
    checks = {}
    for name, path in paths.items():
        try:
            if not path.is_dir():
                checks[name] = {"status": "ERROR", "reason": "directory missing"}
                continue
            usage = shutil.disk_usage(str(path))
            low = usage.free < minimum or usage.free < usage.total * percent / 100
            checks[name] = {
                "status": "LOW_SPACE" if low else "OK",
                "free_bytes": usage.free,
                "total_bytes": usage.total,
            }
        except OSError as error:
            checks[name] = {"status": "ERROR", "reason": str(error)}
    verdict = (
        "ERROR"
        if any(c["status"] == "ERROR" for c in checks.values())
        else "WARNING"
        if any(c["status"] == "LOW_SPACE" for c in checks.values())
        else "PASS"
    )
    return {"role": role, "verdict": verdict, "storage": checks}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--role", choices=["server", "worker"], default="server")
    args = parser.parse_args()
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "worker"))
    from runtime_config import load_config

    try:
        result = storage_health(load_config(args.role), args.role)
        print(json.dumps(result, sort_keys=True))
        sys.exit(0 if result["verdict"] == "PASS" else 1)
    except (ValueError, OSError) as error:
        print("[HEALTH] ERROR: " + str(error), file=sys.stderr)
        sys.exit(2)
