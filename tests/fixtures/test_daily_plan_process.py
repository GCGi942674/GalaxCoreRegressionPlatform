"""Clock plan precedes submission; real Server, partial ACK and restart, no EDA."""
import importlib.util
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
from unittest import mock
from test_sqlite_worker import request, wait_server, stop

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("daily_clock", str(ROOT / "server/clock.py"))
clock = importlib.util.module_from_spec(spec)
spec.loader.exec_module(clock)

with tempfile.TemporaryDirectory(prefix="gcrp-daily-plan-") as folder:
    base = Path(folder)
    for name in ("one", "two"):
        case = base / "test2" / name / "run.tcl"
        case.parent.mkdir(parents=True)
        case.write_text("# fixture")
    (base / "templates").mkdir()
    (base / "templates/route.json").write_text(json.dumps(dict(flow_config=dict(route_design=1))))
    (base / "zips").mkdir()
    (base / "zips/GalaxCore_58231.zip").write_bytes(b"offline fixture, never executed")
    (base / "Tasks.yaml").write_text("tasks:\n" + "".join(
        "  - template: route\n    name: batch{}\n    target: {}\n".format(i, base / "test2") for i in range(3)))
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    config = dict({"network.server_ip":"127.0.0.1", "server.bind_ip":"127.0.0.1",
        "server.port":str(port), "server.io_threads":"2", "server.worker_threads":"2",
        "server.templates_dir":str(base / "templates"), "server.work_root":str(base / "test2"),
        "server.tasks_yaml":str(base / "Tasks.yaml"), "server.artifact_root":str(base / "zips"),
        "database.path":str(base / "state.db"), "database.backup_enabled":"false",
        "server.protect_versions_file":str(base / "protected"), "server.clock_state_file":str(base / "clock.json"),
        "log.dir":str(base / "logs"), "log.level":"ERROR", "server.log_archive_root":str(base / "archive")})
    conf = base / "server.conf"
    conf.write_text("\n".join(k + "=" + v for k, v in config.items()) + "\n")
    env = dict(os.environ, GCRP_CONFIG=str(conf))
    server = None
    with (base / "console.log").open("w") as output:
        try:
            def start():
                process = subprocess.Popen([sys.argv[1]], cwd=str(base), env=env, stdout=output, stderr=subprocess.STDOUT)
                wait_server(port, process)
                return process
            server = start()
            original = clock.request
            sent = []
            def lose_ack(config, kind, data):
                result = original(config, kind, data)
                if kind == "submit_task":
                    sent.append(result["task_id"])
                    if len(sent) == 1:
                        raise TimeoutError("first accepted ACK lost")
                return result
            with mock.patch.object(clock, "request", side_effect=lose_ack):
                try:
                    clock.run(config, interval=0)
                    raise AssertionError("missing injected failure")
                except TimeoutError:
                    pass
            day = clock.date.today().isoformat()
            partial = request(port, "query_daily_plan", dict(date=day))["result"]
            assert [b["planned"] for b in partial["items"]] == [2, 2, 2], partial
            assert [b["submitted"] for b in partial["items"]] == [1, 0, 0], partial
            assert not any(b["complete"] for b in partial["items"]), partial
            clock.run(config, interval=0)
            result = request(port, "query_daily_plan", dict(date=day, limit=2))["result"]
            assert result["has_more"] and result["total"] == 3, result
            assert all(b["submitted"] == 2 and b["not_submitted"] == 0 for b in result["items"]), result
            end = request(port, "query_daily_plan", dict(date=day, offset=result["next_offset"]))["result"]
            assert len(end["items"]) == 1 and not end["has_more"], end
            assert request(port, "query_status", {})["result"]["total"] == 6
            plan = json.loads((base / "clock.json.plan.json").read_text())
            for group in partial["items"]:
                request(port, "cancel_batch", dict(batch_id=group["batch_id"]))
            finished = request(port, "query_daily_plan", dict(date=day))["result"]
            assert all(b["state"] == "STOPPED" and b["cancelled"] == 2 for b in finished["items"]), finished
            conflict = dict(date=day, suite_id="different", plan_sha256="a"*64,
                batches=[dict(batch_id="fake", template="route", name="fake", revision="58231", planned=1)])
            assert request(port, "publish_daily_plan", conflict)["code"] == 1
            assert request(port, "query_daily_plan", dict(date="2026-02-30"))["code"] == 1
            # A manual task does not enter the scheduled plan, even if its batch ID is copied.
            manual = dict(plan[0])
            manual["payload"] = json.loads(json.dumps(manual["payload"]))
            manual["payload"]["context"]["source"] = "manual"
            manual["payload"]["context"]["batch_id"] = "manual-batch"
            manual["submission_key"] = "manual-reproduction"
            assert request(port, "submit_task", manual)["code"] == 0
            stop(server)
            server = start()
            restored = request(port, "query_daily_plan", dict(date=day))["result"]
            assert restored["items"] == finished["items"], (restored, finished)
            assert request(port, "query_daily_plan", dict(date="2000-01-01"))["result"]["plan_available"] is False
            # A published but wholly unsubmitted batch can also be stopped.
            future = dict(date="2000-01-01", suite_id="empty-suite", plan_sha256="b"*64,
                batches=[dict(batch_id="empty-suite-task-1", template="route", name="not submitted", revision="58231", planned=2)])
            assert request(port, "publish_daily_plan", future)["code"] == 0
            empty = request(port, "query_batch", dict(batch_id="empty-suite-task-1"))
            assert empty["code"] == 0 and empty["result"]["total"] == 0, empty
            assert request(port, "cancel_batch", dict(batch_id="empty-suite-task-1"))["code"] == 0
            late = json.loads(json.dumps(plan[0]))
            late["payload"]["context"].update(date="2000-01-01", batch_id="empty-suite-task-1")
            late["submission_key"] = "late-empty-batch"
            assert request(port, "submit_task", late)["code"] == 3
            empty = request(port, "query_daily_plan", dict(date="2000-01-01"))["result"]["items"][0]
            assert empty["state"] == "STOPPED" and empty["not_submitted"] == 2 and not empty["complete"], empty
        finally:
            stop(server)
print("DAILY_PLAN_PROCESS PASS")
