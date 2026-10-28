#!/usr/bin/env python3
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile
import copy

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "worker"))
from process_executor import ProcessExecutor
from pjtest_adapter import PJtestAdapter
from timeout_metrics import select_timeout, valid_timing

spec = importlib.util.spec_from_file_location(
    "vivado_import", str(ROOT / "tools/import_vivado_runtime.py")
)
vivado = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vivado)


class TimingTests(unittest.TestCase):
    def test_guard_normal_and_timeout(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            executor = ProcessExecutor(lambda: None)
            with (root / "out").open("wb") as out, (root / "err").open("wb") as err:
                _, code, timed = executor.run(
                    [sys.executable, "-c", "import time;time.sleep(.12)"],
                    root,
                    os.environ.copy(),
                    out,
                    err,
                    2,
                    0.1,
                )
                self.assertEqual(code, 0)
                self.assertFalse(timed)
                first = dict(executor.last_completion)
                self.assertGreaterEqual(first["command_wall_ms"], 100)
                self.assertLess(first["command_wall_ms"], 1800)
                code_text = (
                    "import os,time;open('child.pid','w').write(str(os.getpid()));time.sleep(10)"
                )
                _, code, timed = executor.run(
                    [sys.executable, "-c", code_text], root, os.environ.copy(), out, err, 0.2, 0.1
                )
                self.assertEqual(code, 124)
                self.assertTrue(timed)
                self.assertTrue(executor.safe)
                self.assertEqual(executor.last_completion["reason"], "timeout")
                self.assertGreaterEqual(executor.last_completion["command_wall_ms"], 180)
                pid = int((root / "child.pid").read_text())
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)

    def test_baseline_index_scope(self):
        import json
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            work = root / "test2"
            case = work / "family/part/case+name/run.tcl"
            case.parent.mkdir(parents=True)
            case.write_text("# fake testcase")
            templates = root / "templates"
            templates.mkdir()
            flow = dict(place_design=1, checksum_cmp=1)
            (templates / "place_design.json").write_text(json.dumps(dict(flow_config=flow)))
            (templates / "route_design.json").write_text(json.dumps(dict(
                flow_config=dict(route_design=1, write_bitstream=1))))
            baseline = root / "baseline"
            log = baseline / "family/part/case+name/place_design.log"
            log.parent.mkdir(parents=True)
            log.write_text("place_design: Time (s): cpu = 00:08:00 ; elapsed = 00:01:20 . Memory (MB): peak = 1")
            index, report = vivado.build_index(dict(
                **{"server.work_root":str(work), "server.templates_dir":str(templates),
                   "server.vivado_baseline_root":str(baseline)}))
            self.assertEqual(report["imported"], 1)
            self.assertEqual(report["skipped"], 1)
            self.assertEqual(index["entries"][0]["elapsed_ms"], 80000)
            self.assertEqual(index["entries"][0]["workload_key"],
                vivado.workload_key("family/part/case+name/run.tcl", flow))
            self.assertIn("scope_mismatch", report["examples"][0]["reason"])

    def test_guard_maintenance_requires_stopped_server(self):
        import fcntl
        import sqlite3
        import subprocess
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            database = root / "state.db"
            connection = sqlite3.connect(str(database))
            connection.execute("CREATE TABLE timeout_guards(guard_key TEXT PRIMARY KEY,run_id TEXT,created_at_ms INTEGER)")
            connection.execute("INSERT INTO timeout_guards VALUES('test-key','test-run',1)")
            connection.commit()
            connection.close()
            config = root / "server.conf"
            config.write_text("database.path=" + str(database) + "\ndatabase.backup_root=" + str(root / "backups") + "\n")
            command = [sys.executable, "-B", str(ROOT / "tools/timeout_guards.py"),
                "--config", str(config), "--clear", "test-key", "--confirm"]
            lock = open(str(database) + ".lock", "w")
            try:
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b"stop Server", result.stderr)
            finally:
                lock.close()
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(list((root / "backups").glob("*.db")))
            connection = sqlite3.connect(str(database))
            self.assertEqual(connection.execute("SELECT COUNT(*) FROM timeout_guards").fetchone()[0], 0)
            connection.close()

    def test_timeout_validation(self):
        task = dict(
            run_id="r1",
            _timeout_v1=True,
            payload=dict(timeout_seconds=5400),
            timeout_decision=dict(
                version=1,
                mode="enforce",
                applied=True,
                decision_id="r1",
                requested_seconds=5400,
                effective_seconds=300,
                expected_fingerprint="a" * 64,
                expected_archive_sha256="b" * 64,
            ),
        )
        self.assertEqual(select_timeout(task, "a" * 64, "b" * 64), (300, "auto_applied"))
        self.assertEqual(select_timeout(task, "c" * 64, "b" * 64)[0], 5400)
        self.assertEqual(select_timeout(task, "a" * 64, "c" * 64)[0], 5400)
        task["_timeout_v1"] = False
        self.assertEqual(select_timeout(task, "a" * 64, "b" * 64)[0], 5400)

    def test_vivado_parser(self):
        line = (
            "place_design: Time (s): cpu = 00:19:00 ; elapsed = 00:02:03.25 . Memory (MB): peak = 1"
        )
        self.assertEqual(vivado.parse_elapsed(line, "place_design"), (123250, 1))
        for text in (
            line + "\n" + line,
            "Runtime: 12",
            "cpu = 00:00:05",
            line.replace("place_design", "route_design"),
        ):
            with self.assertRaises(ValueError):
                vivado.parse_elapsed(text, "place_design")

    def test_adapter_timing_not_overwritten_by_cleanup(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            slot = root / "slot"
            work = slot / "test2"
            (slot / ".svn").mkdir(parents=True)
            work.mkdir()
            for case in ("pass+case", "hang"):
                directory = work / "family" / "part" / case
                directory.mkdir(parents=True)
                (directory / "run.tcl").write_text("# fake case\n")
            (work / "run_proc.tcl").write_text("# fake shared entry\n")
            (work / "run.sh").write_text(
                '#!/bin/bash\nexec python3 "$(dirname "$0")/fixture_runner.py" "$@"\n'
            )
            (work / "clean.sh").write_text("#!/bin/bash\nsleep 0.05\n")
            (work / "fixture_runner.py").write_text(
                "import os,sys,time\nfrom pathlib import Path\n"
                "case=Path(sys.argv[1]); root=Path(__file__).parent\n"
                "if case.parent.name=='hang': time.sleep(20)\n"
                "time.sleep(.15)\n"
                "status=root/'vivado_runner/runtime/workspaces'/os.environ['VIVADO_RUNNER_NAMESPACE']/'status'/case.parent.relative_to(root)\n"
                "status.mkdir(parents=True)\n"
                "(case.parent/'run').write_text('Checksum all right!\\nRuntime: 0\\n')\n"
                "(status/'result.env').write_text('RUN_TCL='+str(case)+'\\nSTATUS=PASS\\nREASON=CHECKSUM_COMPARE_PASS\\nRET_CODE=0\\nRUNTIME_SEC=1\\n')\n"
            )
            artifacts = root / "artifacts"
            artifacts.mkdir()
            with zipfile.ZipFile(str(artifacts / "GalaxCore_1.zip"), "w") as package:
                package.writestr("GalaxCore", "#!/bin/bash\nexit 0\n")
            output = root / "output"
            config = dict(
                galaxcore_root=str(slot),
                artifact_root=str(artifacts),
                log_root=str(output),
                flow_profiles={},
                require_server_flow=True,
                clean=True,
                terminate_grace_seconds=0.1,
            )
            adapter = PJtestAdapter(config, "fixture", lambda: None)
            task = dict(
                task_id=1,
                task_type="REGRESSION",
                revision="1",
                run_id="one",
                payload=dict(
                    spec_version=2,
                    case="family/part/pass+case/run.tcl",
                    flow="place_design",
                    flow_config=dict(place_design=1, checksum_cmp=1),
                    timeout_seconds=10,
                ),
            )
            result = adapter.execute(task, output / "one")
            self.assertEqual(result["status"], "SUCCEEDED", result)
            self.assertTrue(valid_timing(result["timing"]), result)
            self.assertGreaterEqual(result["timing"]["command_wall_ms"], 140)
            self.assertEqual(result["timing"]["supervisor_reason"], "command_exit")
            self.assertGreater(result["timing"]["finalize_wall_ms"], 20)
            self.assertGreater(result["timing"]["prepare_wall_ms"], 20)
            self.assertEqual(result["timing"]["runner_runtime_ms"], 1000)
            self.assertTrue((output / "one" / "evidence" / "run").is_file())
            task2 = copy.deepcopy(task)
            task2["task_id"] = 2
            task2["run_id"] = "two"
            task2["_timeout_v1"] = True
            task2["timeout_decision"] = dict(
                version=1,
                mode="enforce",
                applied=True,
                decision_id="two",
                requested_seconds=10,
                effective_seconds=1,
                expected_fingerprint="0" * 64,
                expected_archive_sha256=result["timing"]["archive_sha256"],
            )
            second = adapter.execute(task2, output / "two")
            self.assertEqual(second["timing"]["effective_timeout_seconds"], 10)
            self.assertEqual(second["timing"]["fallback_reason"], "script_fingerprint_changed")
            task3 = copy.deepcopy(task)
            task3["task_id"] = 3
            task3["payload"]["case"] = "family/part/hang/run.tcl"
            task3["payload"]["timeout_seconds"] = 1
            third = adapter.execute(task3, output / "three")
            self.assertEqual(third["status"], "TIMEOUT", third)
            self.assertTrue(adapter.executor.safe)
            self.assertEqual(third["timing"]["supervisor_reason"], "timeout")


if __name__ == "__main__":
    unittest.main(verbosity=2)
