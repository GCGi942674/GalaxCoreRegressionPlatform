#!/usr/bin/env python3
"""Submit a PJTest-style Tasks.yaml suite to GCRP once per local day.

Invoke from cron or manually; this process does not stay resident.
"""
import argparse
from datetime import date
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import struct
import sys
import sqlite3
import tempfile
import time
import uuid

try:
    import yaml
except ImportError:
    raise SystemExit("PyYAML is required for Tasks.yaml (python3 -m pip install PyYAML)")

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "worker"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from runtime_config import load_config
from daily_log import daily_log
from backup_database import backup_database

MAX_FRAME = 1024 * 1024
ZIP_NAME = re.compile(r"Galax[Cc]ore_(\d+)\.zip\Z")
SAFE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}\Z")
SAFE_KEY = re.compile(r"[A-Za-z_][A-Za-z0-9_]{0,63}\Z")
TERMINAL = {"SUCCEEDED", "FAILED", "TIMEOUT", "CANCELLED"}


class BatchStopped(RuntimeError):
    pass


def required_path(config, key):
    value = config.get(key)
    if not value:
        raise ValueError("configure " + key)
    return Path(value).expanduser().resolve()


def normalize_revision(value):
    if value is None or str(value).strip().lower() == "latest" or str(value).strip() == "":
        return None
    text = str(value).strip()
    if not text.isdigit():
        raise ValueError("revision must be numeric or latest")
    return str(int(text))


def choose_revision(data, entries, artifact_root):
    fixed = {normalize_revision(entry.get("revision")) for entry in entries}
    fixed.discard(None)
    requested = normalize_revision(data.get("batch_revision", data.get("revision")))
    if requested is None and len(fixed) > 1:
        raise ValueError("Tasks.yaml has conflicting fixed revisions")
    requested = requested or (next(iter(fixed)) if fixed else None)
    available = {}
    for path in artifact_root.iterdir():
        match = ZIP_NAME.fullmatch(path.name)
        if match and path.is_file() and not path.is_symlink():
            available[int(match.group(1))] = path
    if not available:
        raise ValueError("no GalaxCore_N.zip in " + str(artifact_root))
    number = int(requested) if requested else max(available)
    if number not in available:
        raise ValueError("selected ZIP version {} is missing".format(number))
    return str(number), available[number]


def within(root, path):
    path = Path(path).resolve(strict=True)
    try:
        path.relative_to(root)
    except ValueError:
        raise ValueError("target escapes server.work_root: " + str(path))
    return path


def discover_cases(root, target):
    target = Path(target).expanduser()
    if target.is_absolute() and target.is_file():
        listing = within(root, target)
        paths = []
        for line in listing.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            case = Path(line)
            if not case.is_absolute():
                raise ValueError("runlist entry must be an absolute run.tcl path")
            paths.append(within(root, case))
    else:
        if not target.is_absolute() and (target.is_file() or (root / target).is_file()):
            raise ValueError("relative runlist file is unsupported")
        directory = within(root, target if target.is_absolute() else root / target)
        if not directory.is_dir():
            raise ValueError("target must be a directory or an absolute runlist")
        paths = []
        for current, dirs, files in os.walk(str(directory)):
            dirs[:] = sorted(name for name in dirs if name not in
                             (".svn", ".git", "output", "outputs", "result", "results",
                              "report", "reports", "log", "logs", "__pycache__"))
            if "run.tcl" in files:
                paths.append(within(root, Path(current) / "run.tcl"))
    cases = sorted(set(path.relative_to(root).as_posix() for path in paths
                       if path.name == "run.tcl"))
    if not cases:
        raise ValueError("target has no run.tcl: " + str(target))
    if len(cases) > 100000:
        raise ValueError("too many cases in one suite")
    return cases


def load_template(directory, name):
    if not isinstance(name, str) or not SAFE_NAME.fullmatch(name):
        raise ValueError("invalid template name")
    path = directory / (name + ".json")
    if not path.is_file() or path.is_symlink():
        raise ValueError("missing template: " + str(path))
    if path.stat().st_size > 32768:
        raise ValueError("template is too large: " + str(path))
    template = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(template, dict):
        raise ValueError("template must be an object")
    flow = template.get("flow_config")
    if (not isinstance(flow, dict) or not 1 <= len(flow) <= 64 or
            any(not SAFE_KEY.fullmatch(key) or type(value) is not int or
                not 0 <= value <= 1000000 for key, value in flow.items())):
        raise ValueError("invalid template flow_config: " + str(path))
    return template


def build_plan(config):
    yaml_path = required_path(config, "server.tasks_yaml")
    root = required_path(config, "server.work_root")
    artifact_root = required_path(config, "server.artifact_root")
    templates = required_path(config, "server.templates_dir")
    source = yaml.safe_load(yaml_path.read_text(encoding="utf-8"))
    if isinstance(source, list):
        source = {"tasks": source}
    if not isinstance(source, dict) or not isinstance(source.get("tasks"), list) or not source["tasks"]:
        raise ValueError("Tasks.yaml must contain a nonempty tasks list")
    entries = source["tasks"]
    if any(not isinstance(entry, dict) or not entry.get("template") for entry in entries):
        raise ValueError("each task needs a template")
    revision, archive = choose_revision(source, entries, artifact_root)
    batch_id = date.today().isoformat() + "-" + revision
    plan = []
    for task_index, entry in enumerate(entries):
        name = entry["template"]
        template = load_template(templates, name)
        retry = entry.get("max_retry", template.get("max_retry", 0))
        if type(retry) is not int or retry != 0:
            raise ValueError("max_retry requires recovery support; set it to 0 for " + name)
        priority = entry.get("priority", template.get("priority", 0))
        timeout = entry.get("max_time", template.get("max_time", 3600))
        if (type(priority) is not int or not -100 <= priority <= 100 or
                type(timeout) is not int or not 1 <= timeout <= 86400):
            raise ValueError("invalid priority or max_time for " + name)
        target = entry.get("target")
        if not isinstance(target, str) or not target:
            raise ValueError("task target is required for " + name)
        suite = str(entry.get("suite", template.get("suite", "")))
        task_name = str(entry.get("name", template.get("task_name", name)))
        if any(len(value) > 128 for value in (suite, task_name, batch_id)):
            raise ValueError("task context is too long")
        for case in discover_cases(root, target):
            plan.append({"task_type": "REGRESSION", "target": task_name,
                         "revision": revision, "priority": priority,
                         "payload": {"spec_version": 2, "case": case, "flow": name,
                                     "timeout_seconds": timeout,
                                     "flow_config": dict(template["flow_config"]),
                                     "context": {"template": name, "suite": suite,
                                                 "name": task_name, "batch_id": batch_id + "-task-" + str(task_index + 1),
                                                 "source": "scheduled", "date": batch_id[:10]}}})
    if not plan:
        raise ValueError("empty task plan")
    return revision, archive, plan


def recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("GCRP connection closed")
        data.extend(chunk)
    return bytes(data)


def request(config, kind, data):
    host = config.get("network.server_ip")
    port = int(config.get("server.port", 0))
    timeout = float(config.get("network.timeout_ms", 5000)) / 1000
    body = json.dumps({"version": 1, "type": kind, "request_id": 1,
                       "data": data}, separators=(",", ":")).encode("utf-8")
    if len(body) > MAX_FRAME:
        raise ValueError("request exceeds protocol limit")
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(struct.pack("!I", len(body)) + body)
        size = struct.unpack("!I", recv_exact(sock, 4))[0]
        if not 0 < size <= MAX_FRAME:
            raise ValueError("invalid GCRP response frame")
        reply = json.loads(recv_exact(sock, size).decode("utf-8"))
    if (not isinstance(reply, dict) or reply.get("type") != "response" or
            reply.get("request_id") != 1 or not isinstance(reply.get("data"), dict)):
        raise ValueError("invalid GCRP response")
    result = reply["data"]
    if result.get("code") == 3 and result.get("message") == "batch stopped":
        raise BatchStopped("batch stopped")
    if result.get("code") != 0:
        raise RuntimeError("{} rejected: {}".format(kind, result.get("message")))
    return result.get("result")


def atomic_text(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix="." + path.name + "-", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, str(path))
        directory_fd = os.open(str(path.parent), os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def run(config, dry_run=False, check_only=False, interval=30):
    if interval < 0:
        raise ValueError("interval must be nonnegative")
    if dry_run:
        revision, archive, plan = build_plan(config)
        print("DRY RUN: revision={} archive={} cases={}".format(revision, archive, len(plan)))
        return 0
    daily_enabled = True
    def accepted_round(state):
        if not state or state.get("phase") != "submitting":
            return False
        plan = request(config, "query_daily_plan", {"date": state["day"]})
        return plan.get("plan_available", False) and plan.get("suite_id") == state.get("batch_id")
    if not check_only:
        automation = request(config, "query_automation", {})
        daily_enabled = automation.get("daily_enabled", False)
        if not daily_enabled:
            # Off blocks NEW rounds, not a previously accepted interrupted round.
            # Do not create a state directory/lock just to report a disabled switch.
            path = Path(config["server.clock_state_file"]).expanduser().resolve() if config.get("server.clock_state_file") else None
            prior = json.loads(path.read_text(encoding="utf-8")) if path and path.is_file() else None
            if not accepted_round(prior):
                print("SKIPPED: daily regression is disabled on Server", flush=True)
                return 0
    state_path = required_path(config, "server.clock_state_file")
    state_path.parent.mkdir(parents=True, exist_ok=True)
    with (state_path.parent / (state_path.name + ".lock")).open("a+") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("another clock invocation is running")
        previous = json.loads(state_path.read_text(encoding="utf-8")) if state_path.exists() else None
        plan_path = state_path.with_name(state_path.name + ".plan.json")
        resume = bool(previous and previous.get("phase") == "submitting")
        if not check_only and not daily_enabled:
            if not accepted_round(previous):
                print("SKIPPED: daily regression is disabled on Server", flush=True)
                return 0
            print("RESUMING: previously accepted daily round; switch remains disabled", flush=True)
        status = request(config, "query_status", {})
        active = sum(status[key] for key in ("pending", "queued", "assigned", "running"))
        if check_only:
            print("unfinished={} total={}".format(active, status["total"]))
            return 0
        # Independent daytime/manual work must not suppress the nightly round.
        # The previous daily groups are checked below instead of all tasks.
        if previous and not resume:
            if previous.get("phase") not in ("complete", "stopped") or previous.get("uncertain"):
                raise RuntimeError("previous submission uncertain; inspect " + str(state_path))
            if previous.get('group_ids'):
                for group_id in previous['group_ids']:
                    group = request(config, 'query_batch', {'batch_id': group_id})
                    if not group.get('complete'):
                        raise RuntimeError('previous daily task is unfinished: ' + group_id)
            else:
                for task_id in previous.get("task_ids", []):
                    task = request(config, "query_task", {"task_id": task_id})
                    if task["status"] not in TERMINAL:
                        raise RuntimeError("previous task is unfinished: {}".format(task_id))
            if previous.get("day") == date.today().isoformat():
                print("SKIPPED: suite {} today".format(
                    "stopped" if previous.get("phase") == "stopped" else "already submitted"))
                return 0
        if resume:
            if not plan_path.is_file():
                raise RuntimeError("missing saved clock plan: " + str(plan_path))
            plan = json.loads(plan_path.read_text(encoding="utf-8"))
            state = previous
            revision = state["revision"]
            batch_id = state["batch_id"]
            if (not isinstance(plan, list) or len(plan) != state["planned"] or
                    any(entry.get("submission_key") != batch_id + ":" + str(index)
                        for index, entry in enumerate(plan))):
                raise RuntimeError("saved clock plan does not match state")
        else:
            revision, archive, plan = build_plan(config)
            batch_id = date.today().isoformat() + "-" + revision + "-" + uuid.uuid4().hex
            groups = {}
            for entry in plan:
                context = entry["payload"]["context"]
                old_group = context["batch_id"]
                if old_group not in groups:
                    groups[old_group] = batch_id + "-task-" + str(len(groups) + 1)
                context["batch_id"] = groups[old_group]
            for index, entry in enumerate(plan):
                entry["submission_key"] = batch_id + ":" + str(index)
            atomic_text(plan_path, json.dumps(plan, sort_keys=True))
            state = {"day": date.today().isoformat(), "revision": revision,
                     "archive": str(archive), "batch_id": batch_id, "phase": "submitting",
                     "task_ids": [], "uncertain": False, "planned": len(plan),
                     "group_ids": list(groups.values())}
        protect = required_path(config, "server.protect_versions_file")
        if config.get('database.path') and config.get('database.backup_enabled', 'true') == 'true':
            print('[BACKUP] ' + str(backup_database(config)), flush=True)
        atomic_text(protect, revision + "\n")
        atomic_text(state_path, json.dumps(state, sort_keys=True))
        # Publish ALL Tasks.yaml entries before the first testcase. An interrupted
        # submit is visible even when some batches have no task records at all.
        batches = {}
        for entry in plan:
            context = entry["payload"]["context"]
            group = context["batch_id"]
            if group not in batches:
                batches[group] = dict(batch_id=group, template=context["template"],
                    name=context["name"], revision=entry["revision"], planned=0)
            batches[group]["planned"] += 1
        manifest = dict(date=state["day"], suite_id=batch_id,
            plan_sha256=hashlib.sha256(json.dumps(plan, sort_keys=True,
                separators=(",", ":")).encode("utf-8")).hexdigest(),
            batches=list(batches.values()))
        request(config, "publish_daily_plan", manifest)
        print("BATCH: " + batch_id, flush=True)
        for index in range(state.get("next_index", len(state["task_ids"])), len(plan)):
            entry = plan[index]
            group_id = entry["payload"]["context"]["batch_id"]
            if group_id in state.get("stopped_groups", []):
                state["next_index"] = index + 1
                atomic_text(state_path, json.dumps(state, sort_keys=True))
                continue
            state["uncertain"] = True
            atomic_text(state_path, json.dumps(state, sort_keys=True))
            try:
                answer = request(config, "submit_task", entry)
            except BatchStopped:
                state["uncertain"] = False
                state.setdefault("stopped_groups", []).append(group_id)
                state["next_index"] = index + 1
                atomic_text(state_path, json.dumps(state, sort_keys=True))
                print("STOPPED: task {}; continuing other Tasks.yaml entries".format(group_id), flush=True)
                continue
            task_id = answer["task_id"]
            state["task_ids"].append(task_id)
            state["next_index"] = index + 1
            state["uncertain"] = False
            atomic_text(state_path, json.dumps(state, sort_keys=True))
            if index == 0 or (index + 1) % 100 == 0 or index + 1 == len(plan):
                print("[{}/{}] task_id={} template={} case={}".format(
                    index + 1, len(plan), task_id, entry["payload"]["flow"], entry["payload"]["case"]),
                    flush=True)
            # Tasks.yaml entries are batches. Never sleep between their cases.
            # Keep the historical 30-second pause after each submitted batch.
            if (index + 1 == len(plan) or
                    plan[index + 1]["payload"]["context"]["batch_id"] != group_id):
                time.sleep(interval)
        state["phase"] = "complete"
        atomic_text(state_path, json.dumps(state, sort_keys=True))
        print("SUBMITTED: {} testcase records at revision {}; stopped task groups={}".format(len(state['task_ids']), revision, len(state.get('stopped_groups', []))))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--interval", type=float, default=30,
                        help="seconds to pause after each Tasks.yaml batch, not each testcase")
    args = parser.parse_args()
    try:
        config = load_config("server")
        with daily_log(config):
            try:
                return run(config, args.dry_run, args.check_only, args.interval)
            except (OSError, ValueError, RuntimeError, KeyError, TypeError, sqlite3.Error) as error:
                print("[CLOCK] " + str(error), file=sys.stderr)
                return 1
    except (OSError, ValueError, RuntimeError, KeyError, TypeError) as error:
        print("[CLOCK] " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
