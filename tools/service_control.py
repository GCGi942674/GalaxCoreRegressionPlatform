#!/usr/bin/env python3
"""Linux service launcher for GCRP; compatible with Python 3.6."""
import argparse
import datetime
import fcntl
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time


def identity(pid):
    try:
        stat = Path("/proc/{}/stat".format(pid)).read_text()
        fields = stat[stat.rfind(")") + 2 :].split()
        if fields[0] == "Z":
            return None
        return fields[19]
    except (OSError, IndexError):
        return None


def active(record, root, role):
    if not isinstance(record, dict):
        return False
    pid = record.get("pid")
    if not isinstance(pid, int) or pid <= 1:
        return False
    if record.get("root") != str(root) or record.get("role") != role:
        return False
    if identity(pid) != record.get("start_ticks"):
        return False
    try:
        args = Path("/proc/{}/cmdline".format(pid)).read_bytes().split(b"\0")
        marker = str(
            root / ("worker/slot_manager.py" if role == "worker" else "build/bin/gcrp_server")
        ).encode()
        return marker in args
    except OSError:
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("role", choices=["server", "worker"])
    parser.add_argument("action", choices=["start", "stop", "status", "reload"])
    parser.add_argument("--config", help="override config via GCRP_CONFIG")
    parser.add_argument(
        "--wait", type=int, default=30, help="seconds to wait for graceful stop (never SIGKILL)"
    )
    args = parser.parse_args()
    if args.wait < 0:
        parser.error("--wait must not be negative")
    root = Path(__file__).resolve().parent.parent
    # Resolve explicit paths before changing cwd; all config-relative paths then
    # have the same repository-root meaning as the documented foreground command.
    explicit = args.config if args.config is not None else os.environ.get("GCRP_CONFIG")
    if explicit is not None:
        if not explicit:
            parser.error("GCRP_CONFIG is empty")
        explicit = str(Path(explicit).expanduser().resolve())
    os.chdir(str(root))
    state_dir = root / "data" / "services"
    state_dir.mkdir(parents=True, exist_ok=True)
    record_path = state_dir / (args.role + ".json")
    with (state_dir / (args.role + ".lock")).open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print("[SERVICE] another {} operation is in progress".format(args.role))
            return 1
        try:
            record = json.loads(record_path.read_text()) if record_path.exists() else {}
        except (OSError, ValueError):
            print("[SERVICE] invalid service record; refusing to overwrite: " + str(record_path))
            return 1
        running = active(record, root, args.role)
        if args.action == "status":
            print(
                "[SERVICE] {} {}{}".format(
                    args.role,
                    "RUNNING" if running else "STOPPED",
                    " pid={} log={}".format(record["pid"], record.get("log", ""))
                    if running
                    else "",
                )
            )
            print("[SERVICE] process status only; use GUI/API to confirm registration/readiness")
            return 0 if running else 3
        if args.action in ("stop", "reload"):
            if not running:
                print("[SERVICE] not running under this launcher; no signal sent")
                return 0 if args.action == "stop" else 1
            if args.action == "reload":
                if args.role != "worker":
                    parser.error("reload is supported only by the Worker manager")
                os.kill(record["pid"], signal.SIGHUP)
                print("[SERVICE] reload requested; inspect manager log for accepted slot count")
                return 0
            os.kill(record["pid"], signal.SIGTERM)
            deadline = time.monotonic() + args.wait
            while active(record, root, args.role):
                if time.monotonic() >= deadline:
                    print(
                        "[SERVICE] draining/shutting down; still running. No forced kill. "
                        "Use status.sh or stop.sh --wait 600 later."
                    )
                    return 2
                time.sleep(0.2)
            record_path.unlink()
            print("[SERVICE] {} stopped".format(args.role))
            return 0
        if running:
            print("[SERVICE] already running pid={}".format(record["pid"]))
            return 0
        config = (
            Path(explicit)
            if explicit
            else next(
                (
                    root / "config" / name
                    for name in (
                        ("worker.config", "worker.conf", "gcrp.conf")
                        if args.role == "worker"
                        else ("server.conf", "gcrp.conf")
                    )
                    if (root / "config" / name).is_file()
                ),
                None,
            )
        )
        if config is None or not config.is_file():
            parser.error("missing config/server.conf or config/worker.config for selected role")
        if args.role == "worker":
            command = [sys.executable, "-B", "-u", str(root / "worker/slot_manager.py")]
            host = "".join(c if c.isalnum() or c in "-_." else "_" for c in socket.gethostname())
            log_root = root / "logs/workers" / host
        else:
            binary = root / "build/bin/gcrp_server"
            if not binary.is_file() or not os.access(str(binary), os.X_OK):
                parser.error("build/bin/gcrp_server is missing/not executable; run make first")
            command = [str(binary)]
            log_root = root / "logs/server"
        day = datetime.datetime.now().strftime("%Y%m%d")
        log_dir = log_root / day
        log_dir.mkdir(parents=True, exist_ok=True)
        log_path = log_dir / (args.role + "-service.log")
        env = os.environ.copy()
        env["GCRP_CONFIG"] = str(config)
        values = {}
        for line in config.read_text(encoding="utf-8").splitlines():
            if "=" in line and not line.lstrip().startswith("#"):
                key, value = line.split("=", 1)
                values[key.strip()] = value.strip()
        days = int(values.get("log.retention_days", "30"))
        maximum = int(values.get("log.service_max_bytes", "10485760"))
        if not 1 <= days <= 3650 or not 65536 <= maximum <= 1073741824:
            parser.error("invalid log retention days or service_max_bytes")
        sink = subprocess.Popen(
            [
                sys.executable,
                "-B",
                str(root / "tools/service_log.py"),
                str(log_root),
                args.role,
                str(days),
                str(maximum),
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        try:
            child = subprocess.Popen(
                command,
                cwd=str(root),
                env=env,
                stdin=subprocess.DEVNULL,
                stdout=sink.stdin,
                stderr=subprocess.STDOUT,
                close_fds=True,
                start_new_session=True,
            )
        finally:
            sink.stdin.close()
        ticks = identity(child.pid)
        record = dict(
            pid=child.pid,
            start_ticks=ticks,
            root=str(root),
            role=args.role,
            log=str(log_root / "*" / (args.role + "-service.*.log")),
            config=str(config),
        )
        temp = record_path.with_suffix(".tmp")
        try:
            temp.write_text(json.dumps(record), encoding="utf-8")
            os.replace(str(temp), str(record_path))
        except OSError:
            child.terminate()
            raise
        time.sleep(1)
        if child.poll() is not None or not active(record, root, args.role):
            record_path.unlink()
            print("[SERVICE] startup failed; inspect " + str(log_path))
            return 1
        print(
            "[SERVICE] {} started pid={} config={} log={}".format(
                args.role, child.pid, config, log_path
            )
        )
        print("[SERVICE] background process started; registration/readiness may still be pending")
        return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print("[SERVICE] " + str(error), file=sys.stderr)
        sys.exit(1)
