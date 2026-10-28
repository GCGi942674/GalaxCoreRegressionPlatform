import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "worker"))
from log_retention import prune_worker_logs, remove_task_artifact
from gcrp_worker import Worker


class ArtifactCleanupTests(unittest.TestCase):
    def task(self, root):
        task = root / "run-test" / "5443"
        task.mkdir(parents=True)
        (task / "artifact.zip").write_bytes(b"private zip")
        (task / "execution.json").write_text("{}")
        return task

    def archived(self, task):
        (task / "archived.json").write_text(json.dumps(dict(
            server_verified=True, verified_files={"stdout.log": {"size": 1}}
        )))

    def test_verified_history_reclaimed_without_waiting_and_idempotent(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            task = self.task(root)
            self.archived(task)
            self.assertEqual(prune_worker_logs(root), 0)
            self.assertFalse((task / "artifact.zip").exists())
            self.assertTrue((task / "execution.json").exists())
            self.assertTrue((task / "archived.json").exists())
            self.assertEqual(prune_worker_logs(root), 0)

    def test_unverified_and_pending_runs_preserved(self):
        for kind in (None, "active-test.json", "pending-report-test.json",
                     "pending-logs-test.json"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                task = self.task(root)
                if kind:
                    self.archived(task)
                    (root / kind).write_text(json.dumps(dict(
                        task_id=5443, directory=str(task))))
                prune_worker_logs(root)
                self.assertTrue((task / "artifact.zip").exists())

    def test_symlink_and_directory_artifacts_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            task = self.task(root)
            archive = task / "artifact.zip"
            archive.unlink()
            source = root / "source.zip"
            source.write_bytes(b"source")
            archive.symlink_to(source)
            with self.assertRaises(RuntimeError):
                remove_task_artifact(task)
            self.assertEqual(source.read_bytes(), b"source")
            archive.unlink()
            archive.mkdir()
            with self.assertRaises(RuntimeError):
                remove_task_artifact(task)

    def test_execute_requires_healthy_adapter_and_durable_report(self):
        for healthy, report_fails in ((True, False), (False, False), (True, True)):
            with self.subTest(healthy=healthy, report_fails=report_fails):
                with tempfile.TemporaryDirectory() as temp:
                    root = Path(temp)
                    task = root / "5443"
                    task.mkdir()
                    archive = task / "artifact.zip"
                    archive.write_bytes(b"zip")
                    worker = Worker.__new__(Worker)
                    worker.assigned = dict(task_id=5443, run_id="run-test")
                    worker.args = SimpleNamespace(worker_id="test", output=root)
                    worker.active_path = lambda: root / "active-test.json"
                    worker.request = Mock()
                    worker.acknowledge_drain = Mock()
                    worker.adapter = SimpleNamespace(
                        healthy=healthy, execute=lambda a, d: dict(status="COMPLETED"))
                    def save(report):
                        self.assertTrue(archive.exists())
                        if report_fails:
                            raise OSError("disk full")
                    worker.save_report = save
                    worker.sock = None
                    # Let execute choose this task directory itself.
                    archive.unlink()
                    task.rmdir()
                    def execute(a, directory):
                        directory.mkdir()
                        (directory / "artifact.zip").write_bytes(b"zip")
                        return dict(status="COMPLETED")
                    worker.adapter.execute = execute
                    if not healthy or report_fails:
                        with self.assertRaises((RuntimeError, OSError)):
                            worker.execute(root)
                        self.assertTrue(archive.exists())
                    else:
                        worker.execute(root)
                        self.assertFalse(archive.exists())


if __name__ == "__main__":
    unittest.main()
