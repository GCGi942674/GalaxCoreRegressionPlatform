"""Offline tests for PJTest-style scheduled submissions."""
import importlib.util
import json
from pathlib import Path
import tempfile
import sys
import unittest
from unittest import mock

import yaml

MODULE = Path(__file__).resolve().parents[2] / "server/clock.py"
spec = importlib.util.spec_from_file_location("gcrp_clock", str(MODULE))
clock = importlib.util.module_from_spec(spec)
spec.loader.exec_module(clock)


class ClockTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gcrp-clock-")
        self.root = Path(self.temporary.name)
        self.work = self.root / "test2"
        case = self.work / "cases/smoke/run.tcl"
        case.parent.mkdir(parents=True)
        case.write_text("# fixture\n")
        self.runlist = self.work / "runlist"
        self.runlist.write_text(str(case) + "\n" + str(case) + "\n")
        self.templates = self.root / "templates"
        self.templates.mkdir()
        (self.templates / "route.json").write_text(json.dumps({
            "task_name": "route", "max_retry": 0,
            "flow_config": {"route_design": 1, "write_bitstream": 1}}))
        self.artifacts = self.root / "artifacts"
        self.artifacts.mkdir()
        (self.artifacts / "GalaxCore_18920.zip").write_bytes(b"fixture")
        self.yaml = self.root / "Tasks.yaml"
        self.yaml.write_text(yaml.safe_dump({"tasks": [{"template": "route",
            "target": str(self.runlist), "suite": "daily_regression"}]}))
        self.config = {"server.tasks_yaml": str(self.yaml),
                       "server.work_root": str(self.work),
                       "server.artifact_root": str(self.artifacts),
                       "server.templates_dir": str(self.templates),
                       "server.protect_versions_file": str(self.root / "protect_versions"),
                       "server.clock_state_file": str(self.root / "state.json"),
                       "network.server_ip": "127.0.0.1", "server.port": "18080"}

    def tearDown(self):
        self.temporary.cleanup()

    def test_plan_pins_version_and_flow(self):
        revision, archive, plan = clock.build_plan(self.config)
        self.assertEqual(revision, "18920")
        self.assertEqual(archive.name, "GalaxCore_18920.zip")
        self.assertEqual(len(plan), 1)
        self.assertEqual(plan[0]["payload"]["case"], "cases/smoke/run.tcl")
        self.assertEqual(plan[0]["payload"]["flow_config"]["route_design"], 1)
        context = plan[0]["payload"]["context"]
        self.assertEqual(context["source"], "scheduled")
        self.assertEqual(context["date"], clock.date.today().isoformat())
        self.assertEqual(context["template"], "route")
        # The unmodified execution layer can consume the newly tagged plan.
        sys.path.insert(0, str(MODULE.parents[1] / "worker"))
        from pjtest_adapter import PJtestAdapter, AdapterError
        adapter = PJtestAdapter.__new__(PJtestAdapter)
        adapter.work_root = self.work.resolve()
        adapter.profiles = {}
        adapter.galaxcore_root = None
        adapter.require_server_flow = True
        task = dict(plan[0], task_id=1)
        case, settings = adapter.validate(task)
        self.assertEqual(case.name, "run.tcl")
        self.assertEqual(settings, plan[0]["payload"]["flow_config"])
        task["payload"]["context"]["extra"] = "unsupported"
        with self.assertRaises(AdapterError):
            adapter.validate(task)
        self.assertEqual(plan[0]["payload"]["context"]["suite"], "daily_regression")

    def test_missing_template_blocks_whole_suite(self):
        self.yaml.write_text(yaml.safe_dump({"tasks": [
            {"template": "route", "target": str(self.runlist)},
            {"template": "missing", "target": str(self.runlist)}]}))
        with self.assertRaisesRegex(ValueError, "missing template"):
            clock.build_plan(self.config)
        self.assertFalse((self.root / "protect_versions").exists())

    def test_default_interval_is_per_yaml_batch_not_per_case(self):
        case = self.work / 'cases/second/run.tcl'
        case.parent.mkdir(parents=True)
        case.write_text('# second case\n')
        self.yaml.write_text(yaml.safe_dump({'tasks': [
            {'template': 'route', 'target': str(self.work / 'cases')}
            for _ in range(5)]}))
        accepted = []
        def reply(_, kind, data):
            if kind == 'query_status':
                return dict(pending=0, queued=0, assigned=0, running=0, total=0)
            if kind == 'publish_daily_plan':
                self.assertEqual([b['planned'] for b in data['batches']], [2] * 5)
                return dict(published=True)
            accepted.append(data['payload']['context']['batch_id'])
            return dict(task_id=len(accepted))
        with mock.patch.object(clock, 'request', side_effect=reply), mock.patch.object(clock.time, 'sleep') as sleep:
            self.assertEqual(clock.run(self.config), 0)
        self.assertEqual(len(accepted), 10)
        self.assertEqual(len(set(accepted)), 5)
        self.assertEqual([call[0][0] for call in sleep.call_args_list], [30] * 5)

    def test_submit_once_and_preserve_uncertain_attempt(self):
        calls = []
        def reply(_, kind, data):
            calls.append(kind)
            if kind == "query_status":
                return {"pending": 0, "queued": 0, "assigned": 0, "running": 0, "total": 0}
            if kind == "query_task":
                return {"status": "SUCCEEDED"}
            if kind == "query_batch":
                return {"complete": True}
            if kind == "publish_daily_plan":
                return {"published": True}
            self.assertEqual(data["payload"]["spec_version"], 2)
            return {"task_id": 101}
        with mock.patch.object(clock, "request", side_effect=reply):
            self.assertEqual(clock.run(self.config, interval=0), 0)
            self.assertEqual(clock.run(self.config, interval=0), 0)
        self.assertEqual(calls.count("submit_task"), 1)
        self.assertEqual((self.root / "protect_versions").read_text(), "18920\n")
        state = json.loads((self.root / "state.json").read_text())
        self.assertEqual(state["task_ids"], [101])
        self.assertEqual(state["phase"], "complete")

        (self.root / "state.json").unlink()
        replay_keys = []
        def lost_ack(_, kind, data):
            if kind == "query_status":
                return {"pending": 0, "queued": 0, "assigned": 0, "running": 0, "total": 0}
            if kind == "publish_daily_plan":
                return {"published": True}
            replay_keys.append(data["submission_key"])
            raise TimeoutError("ack lost")
        with mock.patch.object(clock, "request", side_effect=lost_ack):
            with self.assertRaises(TimeoutError):
                clock.run(self.config, interval=0)
        self.assertTrue(json.loads((self.root / "state.json").read_text())["uncertain"])
        def recovered(_, kind, data):
            if kind == "query_status":
                return {"pending": 0, "queued": 0, "assigned": 0, "running": 0, "total": 0}
            if kind == "publish_daily_plan":
                return {"published": True}
            replay_keys.append(data["submission_key"])
            return {"task_id": 102}
        with mock.patch.object(clock, "request", side_effect=recovered):
            self.assertEqual(clock.run(self.config, interval=0), 0)
        self.assertEqual(replay_keys[0], replay_keys[1])
        self.assertEqual(json.loads((self.root / "state.json").read_text())["task_ids"], [102])

    def test_stop_during_submission_records_known_tasks(self):
        self.yaml.write_text(yaml.safe_dump({"tasks": [
            {"template": "route", "target": str(self.runlist)},
            {"template": "route", "target": str(self.runlist)},
            {"template": "route", "target": str(self.runlist)}]}))
        submitted = []
        def reply(_, kind, data):
            if kind == "query_status":
                return {"pending": 0, "queued": 0, "assigned": 0, "running": 0, "total": 0}
            if kind == "query_task":
                return {"status": "SUCCEEDED"}
            if kind == "query_batch":
                return {"complete": True}
            if kind == "publish_daily_plan":
                return {"published": True}
            submitted.append(data["payload"]["context"]["batch_id"])
            if len(submitted) == 2:
                raise clock.BatchStopped("batch stopped")
            return {"task_id": 100 + len(submitted)}
        with mock.patch.object(clock, "request", side_effect=reply):
            self.assertEqual(clock.run(self.config, interval=0), 0)
            self.assertEqual(clock.run(self.config, interval=0), 0)
        state = json.loads((self.root / "state.json").read_text())
        self.assertEqual(state["phase"], "complete")
        self.assertFalse(state["uncertain"])
        self.assertEqual(state["task_ids"], [101, 103])
        self.assertTrue(submitted[0].startswith(state["batch_id"]))
        self.assertNotEqual(submitted[0], submitted[1])
        self.assertEqual(state["stopped_groups"], [submitted[1]])


if __name__ == "__main__":
    unittest.main()
