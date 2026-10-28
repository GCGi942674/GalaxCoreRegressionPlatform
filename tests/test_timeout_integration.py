#!/usr/bin/env python3
import json, os, signal, socket, sqlite3, struct, subprocess, sys, tempfile, time, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv[1])


def wait(check, seconds=15):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        try:
            result = check()
            if result:
                return result
        except (OSError, ValueError, KeyError, AssertionError):
            pass
        time.sleep(0.05)
    raise AssertionError("deadline exceeded")


class Peer:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), 3)
        self.sock.settimeout(5)
        self.index = 0
        self.assignments = []

    def read(self, n):
        data = b""
        while len(data) < n:
            block = self.sock.recv(n - len(data))
            if not block:
                raise ConnectionError("closed")
            data += block
        return data

    def request(self, kind, data):
        self.index += 1
        wire = json.dumps(dict(version=1, request_id=self.index, type=kind, data=data)).encode()
        self.sock.sendall(struct.pack("!I", len(wire)) + wire)
        while True:
            length = struct.unpack("!I", self.read(4))[0]
            assert 0 < length <= 1048576
            frame = json.loads(self.read(length).decode())
            if frame["type"] == "task_assign":
                self.assignments.append(frame["data"])
                continue
            assert frame["request_id"] == self.index, frame
            result = frame["data"]
            assert result["code"] == 0, result
            return result.get("result")

    def close(self):
        self.sock.close()


def main():
    with tempfile.TemporaryDirectory(prefix="gcrp-timeout-e2e-") as temp:
        base = Path(temp)
        slot = base / "slot"
        work = slot / "test2"
        (slot / ".svn").mkdir(parents=True)
        work.mkdir()
        for c in ("pass+case", "fail", "hang"):
            p = work / "family" / "part" / c
            p.mkdir(parents=True)
            (p / "run.tcl").write_text("# fake\n")
        (work / "run_proc.tcl").write_text("# shared\n")
        (work / "run.sh").write_text(
            '#!/bin/bash\nexec python3 "$(dirname "$0")/fixture.py" "$@"\n'
        )
        (work / "clean.sh").write_text("#!/bin/bash\nsleep .02\n")
        (work / "fixture.py").write_text(
            "import os,sys,time\nfrom pathlib import Path\n"
            "c=Path(sys.argv[1]);r=Path(__file__).parent\n"
            "if c.parent.name=='hang':time.sleep(20)\n"
            "time.sleep(.1);state='FAIL' if c.parent.name=='fail' else 'PASS'\n"
            "(c.parent/'run').write_text('RESULT '+state+'\\nRuntime: 1\\n')\n"
            "p=r/'vivado_runner/runtime/workspaces'/os.environ['VIVADO_RUNNER_NAMESPACE']/'status'/c.parent.relative_to(r)\n"
            "p.mkdir(parents=True)\n"
            "(p/'result.env').write_text('RUN_TCL='+str(c)+'\\nSTATUS='+state+'\\nREASON=CHECKSUM_COMPARE_'+state+'\\nRET_CODE=0\\nRUNTIME_SEC=1\\n')\n"
        )
        artifacts = base / "artifacts"
        artifacts.mkdir()
        with zipfile.ZipFile(str(artifacts / "GalaxCore_1.zip"), "w") as z:
            z.writestr("GalaxCore", "#!/bin/bash\nexit 0\n")
        payload = dict(
            spec_version=2,
            case="family/part/pass+case/run.tcl",
            flow="place_design",
            flow_config=dict(place_design=1, checksum_cmp=1),
            timeout_seconds=5400,
        )
        db = base / "state.db"
        con = sqlite3.connect(str(db))
        con.executescript(
            """
CREATE TABLE batches(batch_id TEXT PRIMARY KEY,stop_requested INTEGER NOT NULL DEFAULT 0);
CREATE TABLE tasks(task_id INTEGER PRIMARY KEY AUTOINCREMENT,batch_id TEXT REFERENCES batches(batch_id),task_type TEXT NOT NULL,target TEXT NOT NULL,revision TEXT NOT NULL,priority INTEGER NOT NULL,status TEXT NOT NULL,payload_json TEXT NOT NULL,execution_json TEXT,worker_id TEXT,retry_count INTEGER NOT NULL DEFAULT 0,queue_sequence INTEGER NOT NULL,run_id TEXT,recovery_required INTEGER NOT NULL DEFAULT 0,created_at_ms INTEGER NOT NULL,started_at_ms INTEGER,finished_at_ms INTEGER);
CREATE TABLE task_runs(run_id TEXT PRIMARY KEY,task_id INTEGER NOT NULL REFERENCES tasks(task_id),attempt_no INTEGER NOT NULL,worker_id TEXT NOT NULL,status TEXT NOT NULL,assigned_at_ms INTEGER NOT NULL,started_at_ms INTEGER,finished_at_ms INTEGER,execution_json TEXT,UNIQUE(task_id,attempt_no));
PRAGMA user_version=6;
"""
        )
        summary = json.dumps(dict(reason="pass", case_status="PASS", raw_exit_code=0))
        con.execute(
            "INSERT INTO tasks VALUES(10,NULL,'REGRESSION','legacy','1',0,'SUCCEEDED',?,?,'old-slot',0,10,'legacy-10',0,1,2,3)",
            (json.dumps(payload), summary),
        )
        con.execute(
            "INSERT INTO task_runs VALUES('legacy-10',10,1,'old-slot','SUCCEEDED',1,2,3,?)",
            (summary,),
        )
        con.commit()
        con.close()
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        cfg = base / "server.conf"
        values = dict(
            [
                ("server.bind_ip", "127.0.0.1"),
                ("server.port", port),
                ("server.io_threads", 1),
                ("server.worker_threads", 2),
                ("network.server_ip", "127.0.0.1"),
                ("network.timeout_ms", 5000),
                ("database.path", db),
                ("server.artifact_root", artifacts),
                ("server.protect_versions_file", base / "protected"),
                ("server.log_archive_root", base / "archive"),
                ("log.dir", base / "logs"),
                ("log.level", "WARN"),
                ("policy.enabled", "false"),
                ("server.resource_policy_file", ""),
                ("timeout.mode", "observe"),
                ("timeout.baseline_index_file", base / "absent.json"),
                ("worker.artifact_root", artifacts),
                ("worker.galaxcore_root", slot),
                ("worker.output_dir", base / "worker-output"),
            ]
        )
        cfg.write_text("".join(str(k) + "=" + str(v) + "\n" for k, v in values.items()))
        local = base / "worker.json"
        local.write_text(
            json.dumps(
                dict(
                    galaxcore_root=str(slot),
                    artifact_root=str(artifacts),
                    log_root=str(base / "worker-output"),
                    flow_profiles={},
                    require_server_flow=True,
                    clean=True,
                    terminate_grace_seconds=0.1,
                )
            )
        )
        env = os.environ.copy()
        env["GCRP_CONFIG"] = str(cfg)
        processes = []
        streams = []

        def spawn(args, label):
            stream = (base / label).open("wb")
            streams.append(stream)
            p = subprocess.Popen(
                args, cwd=str(ROOT), env=env, stdout=stream, stderr=subprocess.STDOUT
            )
            processes.append(p)
            return p

        def stop(p):
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
                p.wait(timeout=12)

        def rpc(kind, data):
            p = Peer(port)
            try:
                return p.request(kind, data)
            finally:
                p.close()

        def start_server():
            p = spawn([str(BINARY)], "server-" + str(len(processes)) + ".log")
            wait(lambda: rpc("query_task", dict(task_id=10)))
            return p

        try:
            server = start_server()
            old = rpc("query_task", dict(task_id=10))
            assert old["status"] == "SUCCEEDED" and old["timeout_mode"] == "fixed", old
            assert "timing" not in old["execution"]
            worker = spawn(
                [
                    sys.executable,
                    "-B",
                    "worker/gcrp_worker.py",
                    "--worker-id",
                    "bench-slot1",
                    "--machine-id",
                    "bench",
                    "--slot-id",
                    "slot1",
                    "--pjtest-config",
                    str(local),
                ],
                "worker.log",
            )
            wait(
                lambda: any(
                    w["worker_id"] == "bench-slot1" and w["status"] == "ONLINE"
                    for w in rpc("query_workers", {})["items"]
                )
            )
            ids = []
            for index, c in enumerate(("pass+case", "fail", "hang")):
                data = dict(payload)
                data["case"] = "family/part/" + c + "/run.tcl"
                data["timeout_seconds"] = 1 if c == "hang" else 5400
                request = dict(
                    task_type="REGRESSION",
                    target="place_design",
                    revision="1",
                    priority=0,
                    payload=data,
                    submission_key="test-key-" + str(index),
                    timeout_mode="auto",
                )
                accepted = rpc("submit_task", request)
                assert rpc("submit_task", request)["task_id"] == accepted["task_id"]
                ids.append(accepted["task_id"])
            results = []
            for ident, state in zip(ids, ("SUCCEEDED", "FAILED", "TIMEOUT")):
                r = wait(
                    lambda: (
                        lambda t: t if t["status"] in ("SUCCEEDED", "FAILED", "TIMEOUT") else None
                    )(rpc("query_task", dict(task_id=ident))),
                    20,
                )
                assert r["status"] == state, r
                assert r["timeout_decision"]["applied"] is False, r
                assert (
                    r["execution"]["timing"]["effective_timeout_seconds"]
                    == r["payload"]["timeout_seconds"]
                ), r
                results.append(r)
                if ident == ids[1]:
                    active = wait(lambda: (lambda task: task if task["status"] == "RUNNING" else None)(
                        rpc("query_task", dict(task_id=ids[2]))))
                    stop(server)
                    server = start_server()
                    recovered = rpc("query_task", dict(task_id=ids[2]))
                    assert recovered["run_id"] == active["run_id"]
                    assert recovered["timeout_decision"] == active["timeout_decision"]
            wait(
                lambda: all(
                    (
                        base / "archive" / str(t["task_id"]) / t["run_id"] / "result.json.verified"
                    ).exists()
                    for t in results
                )
            )
            listed = rpc("query_tasks", dict(limit=50))["items"]
            listed_task = next(t for t in listed if t["task_id"] == ids[0])
            assert (
                listed_task["execution"]["timing"] == results[0]["execution"]["timing"]
            ), listed_task
            assert listed_task["timeout_decision"] == results[0]["timeout_decision"], listed_task
            stop(worker)
            stop(server)
            con = sqlite3.connect(str(db))
            assert con.execute("PRAGMA user_version").fetchone()[0] == 7
            assert con.execute("SELECT COUNT(*) FROM runtime_samples").fetchone()[0] == 3
            con.close()
            server = start_server()
            after = rpc("query_task", dict(task_id=ids[0]))
            assert (
                after["run_id"] == results[0]["run_id"]
                and after["timeout_decision"] == results[0]["timeout_decision"]
            )
            assert after["execution"] == results[0]["execution"]
            peer = Peer(port)
            try:
                registered = peer.request(
                    "worker_register",
                    dict(
                        worker_id="bench-slot1",
                        hostname="bench",
                        machine_id="bench",
                        slot_id="slot1",
                        slots=1,
                        timeout_version=1,
                        recovery_version=1,
                    ),
                )
                assert registered["timeout_v1"] is True
                peer.request("worker_recover", dict(worker_id="bench-slot1", state="idle"))
                report = dict(
                    task_id=after["task_id"],
                    run_id=after["run_id"],
                    worker_id="bench-slot1",
                    status=after["status"],
                    message=json.dumps(after["execution"]),
                )
                peer.request("task_result", report)
                rpc("delete_task", dict(task_id=after["task_id"], confirm=True))
                peer.request("task_result", report)
            finally:
                peer.close()
            con = sqlite3.connect(str(db))
            assert con.execute("SELECT COUNT(*) FROM runtime_samples").fetchone()[0] == 3
            con.close()
            print(
                "PASS: schema6 migration; real Server/Worker PASS, FAIL, TIMEOUT; archive; restart; repeated/deleted-result ACK; sample dedup"
            )
        except Exception:
            for file in base.glob("*.log"):
                print("LOG", file.name, file.read_text(errors="replace")[-8000:])
            raise
        finally:
            for p in reversed(processes):
                if p.poll() is None:
                    p.send_signal(signal.SIGTERM)
                    try:
                        p.wait(timeout=12)
                    except subprocess.TimeoutExpired:
                        p.kill()
                        p.wait()
            for stream in streams:
                stream.close()


if __name__ == "__main__":
    main()
