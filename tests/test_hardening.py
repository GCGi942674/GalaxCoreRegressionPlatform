"""Durable log intent and conservative service-log retention; no real tasks."""
import json
import os
from pathlib import Path
import sys
import tempfile
import sqlite3
import time
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'worker'))
sys.path.insert(0, str(ROOT / 'tools'))
from gcrp_worker import Worker, TransportError, result_summary
from log_retention import prune_worker_logs
from recovery_journal import write_json
from service_log import prune
from backup_database import backup_database
from health_check import storage_health
from pjtest_adapter import PJtestAdapter


class HardeningTests(unittest.TestCase):
    def test_lost_local_cleanup_ack_is_replayed_without_uploading_or_deleting_other_files(self):
        with tempfile.TemporaryDirectory() as folder:
            output=Path(folder);directory=output/'run-test'/'1';directory.mkdir(parents=True)
            (directory/'stdout.log').write_bytes(b'verified logs')
            (directory/'artifact.zip').write_bytes(b'preserve')
            worker=Worker(SimpleNamespace(output=output,worker_id='host-slot1',pjtest_config=None,pjtest_paths={}))
            worker.archive_lifecycle=True
            worker.queue_logs(dict(task_id=1,run_id='run-1'),directory)
            phases=[]
            def rpc(kind,data):
                if kind=='log_state':
                    phases.append(data['state']['phase'])
                    if data['state']['phase']=='LOCAL_CLEANED':
                        raise TransportError('cleanup ACK lost')
                    return dict(recorded=True)
                return dict(next_offset=13,complete=True,verified_sha256=data.get('sha256'))
            with mock.patch.object(worker,'request',side_effect=rpc):
                with self.assertRaises(TransportError):worker.flush_logs()
            self.assertEqual(phases,['UPLOADING','LOCAL_CLEANED'])
            self.assertFalse((directory/'stdout.log').exists())
            self.assertTrue((output/'pending-logs-run-1.json').exists())
            with mock.patch.object(worker,'request',return_value=dict(recorded=True)) as replay:
                worker.flush_logs()
                self.assertEqual(replay.call_count,1)
                self.assertEqual(replay.call_args[0][0],'log_state')
                self.assertEqual(replay.call_args[0][1]['state']['phase'],'LOCAL_CLEANED')
            self.assertTrue((directory/'artifact.zip').exists())
            self.assertFalse((output/'pending-logs-run-1.json').exists())

    def test_slow_server_hash_verification_keeps_worker_heartbeats_alive(self):
        with tempfile.TemporaryDirectory() as folder:
            worker=Worker(SimpleNamespace(output=Path(folder),worker_id='host-slot1',pjtest_config=None,pjtest_paths={}))
            replies=[];heartbeats=[];started=time.monotonic();verification_id=[None]
            class Socket:
                def sendall(self,frame):
                    message=json.loads(frame[4:])
                    if message['type']=='worker_heartbeat':
                        heartbeats.append(message['request_id'])
                        replies.append(dict(type='response',request_id=message['request_id'],data=dict(code=0,result={})))
                    else:verification_id[0]=message['request_id']
            def receive(deadline):
                while time.monotonic()<deadline:
                    if replies:return replies.pop(0)
                    if time.monotonic()-started>=5.2:
                        return dict(type='response',request_id=verification_id[0],data=dict(code=0,result={'verified_sha256':'a'*64}))
                    time.sleep(.01)
                raise TimeoutError('waiting for hash')
            worker.sock=Socket()
            with mock.patch.object(worker,'receive',side_effect=receive):
                reply=worker.request('log_append',dict(sha256='a'*64))
            self.assertEqual(reply['verified_sha256'],'a'*64)
            self.assertGreaterEqual(len(heartbeats),3)

    def test_run_capture_is_full_and_requires_a_fresh_execution(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);case=root/'case/run.tcl';case.parent.mkdir();case.write_text('# test')
            source=case.parent/'run';source.write_bytes(b'OLD_TASK')
            adapter=PJtestAdapter.__new__(PJtestAdapter)
            adapter.executor=SimpleNamespace(heartbeat=lambda:None)
            before=adapter._run_identity(source)
            not_started=root/'not-started';not_started.mkdir()
            self.assertFalse(adapter._capture_run(case,not_started,before,False)['captured'])
            unchanged=root/'unchanged';unchanged.mkdir()
            self.assertFalse(adapter._capture_run(case,unchanged,before,True)['captured'])
            source.write_bytes(b'A'*(8*1024*1024)+b'FINAL_FAILURE_PHASE=route_design')
            fresh=root/'fresh';fresh.mkdir()
            info=adapter._capture_run(case,fresh,before,True)
            self.assertTrue(info['complete']);self.assertFalse(info['truncated'])
            self.assertEqual((fresh/'evidence/run').read_bytes(),source.read_bytes())

    def test_compact_result_preserves_pass_with_large_diagnostics(self):
        result=dict(status='SUCCEEDED',case_status='PASS',reason='pass',argv=['x'*20000],payload={'case':'y'*5000})
        summary=result_summary(result)
        self.assertEqual(summary['case_status'],'PASS')
        self.assertLess(len(json.dumps(summary)),1024)

    def test_verified_upload_cleans_only_task_logs_and_resumes_after_cleanup_crash(self):
        with tempfile.TemporaryDirectory() as folder:
            output=Path(folder);directory=output/'run-test'/'1';directory.mkdir(parents=True)
            (directory/'stdout.log').write_bytes(b'original stdout')
            (directory/'stderr.log').write_bytes(b'original stderr')
            (directory/'artifact.zip').write_bytes(b'not-a-log-preserve')
            worker=Worker(SimpleNamespace(output=output,worker_id='host-slot1',pjtest_config=None,pjtest_paths={}))
            worker.queue_logs(dict(task_id=1,run_id='run-1'),directory)
            server={}
            def rpc(kind,data):
                if 'sha256' in data:
                    import hashlib
                    self.assertEqual(hashlib.sha256(server[data['name']]).hexdigest(),data['sha256'])
                    return dict(next_offset=len(server[data['name']]),complete=True,verified_sha256=data['sha256'])
                block=bytes.fromhex(data['data_hex']);server.setdefault(data['name'],b'')
                if data['offset']==len(server[data['name']]):server[data['name']]+=block
                return dict(next_offset=len(server[data['name']]),complete=data['complete'])
            def interrupted_cleanup(entry):
                (directory/'stdout.log').unlink()
                raise OSError('simulated crash during local cleanup')
            with mock.patch.object(worker,'request',side_effect=rpc),mock.patch.object(worker,'cleanup_archived_logs',side_effect=interrupted_cleanup):
                with self.assertRaises(OSError):worker.flush_logs()
            manifest=output/'pending-logs-run-1.json'
            self.assertTrue(json.loads(manifest.read_text())['cleanup_approved'])
            self.assertTrue((directory/'stderr.log').exists())
            with mock.patch.object(worker,'request') as request:
                worker.flush_logs()
                request.assert_not_called()
            self.assertFalse(manifest.exists())
            self.assertFalse((directory/'stderr.log').exists())
            self.assertEqual(server['stdout.log'],b'original stdout')
            self.assertEqual(server['stderr.log'],b'original stderr')
            self.assertTrue((directory/'archived.json').is_file())
            self.assertTrue((directory/'artifact.zip').is_file())

    def test_unverified_server_or_lost_verification_ack_never_deletes_logs(self):
        for failure in ('old-server','lost-ack','wrong-hash'):
            with tempfile.TemporaryDirectory() as folder:
                output=Path(folder);directory=output/'run-test'/'1';directory.mkdir(parents=True)
                source=directory/'stdout.log';source.write_bytes(b'keep this evidence')
                worker=Worker(SimpleNamespace(output=output,worker_id='host-slot1',pjtest_config=None,pjtest_paths={}))
                worker.queue_logs(dict(task_id=1,run_id='run-1'),directory)
                def rpc(kind,data):
                    if 'sha256' in data:
                        if failure=='old-server':raise RuntimeError('unsupported field')
                        if failure=='lost-ack':raise TransportError('ACK lost')
                        return dict(next_offset=source.stat().st_size,complete=True,verified_sha256='0'*64)
                    return dict(next_offset=source.stat().st_size,complete=True)
                with mock.patch.object(worker,'request',side_effect=rpc):
                    with self.assertRaises((RuntimeError,TransportError)):worker.flush_logs()
                self.assertTrue(source.exists())
                self.assertTrue((output/'pending-logs-run-1.json').exists())

    def test_oversized_log_blocks_cleanup_and_upgrades_old_outbox(self):
        with tempfile.TemporaryDirectory() as folder:
            output=Path(folder);directory=output/'run-test'/'1';directory.mkdir(parents=True)
            source=directory/'stdout.log';source.write_bytes(b'must preserve oversized evidence')
            (directory/'stderr.log').write_bytes(b'')
            worker=Worker(SimpleNamespace(output=output,worker_id='host-slot1',pjtest_config=None,pjtest_paths={}))
            manifest=output/'pending-logs-run-1.json'
            write_json(manifest,dict(task_id=1,run_id='run-1',worker_id='host-slot1',directory=str(directory),files=['stderr.log']))
            with mock.patch('gcrp_worker._LOG_LIMIT',1),mock.patch.object(worker,'request') as request:
                worker.queue_logs(dict(task_id=1,run_id='run-1'),directory)
                with self.assertRaises(RuntimeError):worker.flush_logs()
                request.assert_not_called()
            self.assertTrue(source.exists());self.assertTrue((directory/'stderr.log').exists())
            self.assertIn('stdout.log',json.loads(manifest.read_text())['files'])

    def test_retention_preserves_referenced_receipt_and_collects_unreferenced_receipt(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);live=root/('clean-'+'a'*32+'.json');old=root/('clean-'+'b'*32+'.json')
            for path in (live,old):
                write_json(path,dict(clean=True,token=path.name));os.utime(str(path),(1,1))
            write_json(root/'active-test.json',dict(task_id=1,receipt=str(live)))
            prune_worker_logs(root)
            self.assertTrue(live.exists());self.assertFalse(old.exists())

    def test_capacity_warning_without_deleting_data(self):
        with tempfile.TemporaryDirectory() as folder:
            config={'worker.output_root':folder,'worker.slots_root':folder,'health.min_free_bytes':'1000'}
            with mock.patch('health_check.shutil.disk_usage',return_value=SimpleNamespace(free=10,total=10000)):
                result=storage_health(config,'worker')
                self.assertEqual(result['verdict'],'WARNING')
                self.assertEqual(result['storage']['slots']['status'],'LOW_SPACE')
    def test_sqlite_wal_backup_and_python36_fallback(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / 'state.db'
            with sqlite3.connect(str(source)) as writer:
                writer.execute('PRAGMA journal_mode=WAL')
                writer.execute('CREATE TABLE tasks(id INTEGER PRIMARY KEY, result TEXT)')
                writer.execute("INSERT INTO tasks VALUES(1,'PASS')")
                writer.execute('PRAGMA user_version=3')
                writer.commit()
                for logical in (False, True):
                    root = Path(folder) / str(logical)
                    path = backup_database({'database.path': str(source), 'database.backup_root': str(root), 'database.min_free_bytes':'0'}, logical=logical)
                    with sqlite3.connect(str(path)) as restored:
                        self.assertEqual(restored.execute('SELECT result FROM tasks').fetchone()[0], 'PASS')
                        self.assertEqual(restored.execute('PRAGMA user_version').fetchone()[0], 3)
    def test_result_keeps_archive_retry_intent(self):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder)
            directory = output / 'run-test' / '1'
            directory.mkdir(parents=True)
            (directory / 'stdout.log').write_text('evidence')
            worker = Worker(SimpleNamespace(output=output, worker_id='host-slot1',
                pjtest_config=None, pjtest_paths={}))
            assignment = dict(task_id=1, run_id='run-1')
            worker.save_report(dict(task_id=1, run_id='run-1', worker_id='host-slot1',
                status='SUCCEEDED', message='{}',
                _archive=dict(assignment=assignment, directory=str(directory))))
            with mock.patch.object(worker, 'queue_logs', side_effect=OSError('disk full')), mock.patch.object(worker, 'request') as request:
                with self.assertRaises(TransportError):
                    worker.flush_report()
                request.assert_not_called()
            self.assertTrue(worker.pending_path().exists())
            with mock.patch.object(worker, 'request', return_value={}) as request:
                worker.flush_report()
                self.assertNotIn('_archive', request.call_args[0][1])
            self.assertFalse(worker.pending_path().exists())
            self.assertTrue((output / 'pending-logs-run-1.json').is_file())
            worker.queue_logs(assignment, directory)  # Same intent is idempotent.

    def test_service_retention_preserves_live_and_unknown_files(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            day = root / '20000101'
            day.mkdir()
            expired = day / 'worker-service.1.log'
            live = day / 'worker-service.2.log'
            unknown = day / 'important.log'
            for path in (expired, live, unknown):
                path.write_text('log')
                os.utime(str(path), (1, 1))
            with live.open('rb'):
                prune(root, 'worker', 30)
                self.assertTrue(live.exists())
            self.assertFalse(expired.exists())
            self.assertTrue(unknown.exists())


if __name__ == '__main__':
    unittest.main()
