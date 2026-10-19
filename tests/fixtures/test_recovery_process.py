"""Fault injection with real TCP, SQLite, Python Worker and Linux processes."""
import json
import os
from pathlib import Path
import socket
import signal
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time

from test_sqlite_worker import request, wait_server, stop


def eventually(check, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        time.sleep(.05)
    raise AssertionError('condition timed out')


class Rig:
    def __init__(self, root, server_bin, worker_script):
        self.root, self.server_bin, self.worker_script = root, server_bin, worker_script
        self.server = self.worker = None
        self.logs = []
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            self.port = probe.getsockname()[1]
        self.config = root / 'config'
        self.config.write_text(
            'server.bind_ip=127.0.0.1\nserver.port={}\nnetwork.server_ip=127.0.0.1\n'
            'server.io_threads=2\nserver.worker_threads=2\nlog.level=WARN\n'
            'log.dir={}\ndatabase.path={}\nworker.output_dir={}\n'
            'worker.reconnect_seconds=0.1\nserver.recovery_max_retries=1\nserver.worker_timeout_seconds=2\n'.format(
                self.port, root / 'logs', root / 'state.db', root / 'output'))
        self.env = dict(os.environ, GCRP_CONFIG=str(self.config))

    def spawn(self, argv):
        output = (self.root / ('process-{}.log'.format(len(self.logs)))).open('wb')
        self.logs.append(output)
        return subprocess.Popen(argv, cwd=str(self.root), env=self.env,
                                stdout=output, stderr=subprocess.STDOUT)

    def start_server(self):
        self.server = self.spawn([self.server_bin])
        wait_server(self.port, self.server)

    def start_worker(self, duration):
        self.worker = self.spawn([sys.executable, '-B', '-u', self.worker_script,
            '--worker-id', 'recover-slot', '--demo', '--demo-seconds', str(duration),
            '--task-timeout', '40'])

    def submit(self):
        reply = request(self.port, 'submit_task', dict(task_type='REGRESSION',
                        target='recovery', revision='18925', payload={}))
        assert reply['code'] == 0, reply
        return reply['result']['task_id']

    def task(self, task_id):
        reply = request(self.port, 'query_task', dict(task_id=task_id))
        assert reply['code'] == 0, reply
        return reply['result']

    def journal(self):
        paths = list((self.root / 'output').glob('active-*.json'))
        if not paths:
            return None
        return paths[0], json.loads(paths[0].read_text())

    def executing(self, task_id):
        if self.task(task_id)['status'] != 'RUNNING':
            return None
        journal = self.journal()
        launched = list((self.root / 'output').glob('run-*/{}/pid'.format(task_id)))
        return journal if journal and journal[1]['receipt'] and launched else None

    def succeeded(self, task_id):
        value = self.task(task_id)
        return value if value['status'] == 'SUCCEEDED' else None

    def close(self):
        stop(self.worker)
        stop(self.server)
        for output in self.logs:
            output.close()


def main():
    with tempfile.TemporaryDirectory(prefix='gcrp-p12-') as directory:
        rig = Rig(Path(directory), *sys.argv[1:3])
        try:
            rig.start_server()
            rig.start_worker(4)
            first = rig.submit()
            eventually(lambda: rig.executing(first))
            duplicate = rig.spawn([sys.executable, '-B', rig.worker_script,
                '--worker-id', 'recover-slot', '--demo'])
            assert duplicate.wait(timeout=5) == 1
            assert rig.worker.poll() is None
            original = rig.task(first)
            rig.server.kill()
            rig.server.wait(timeout=5)
            # Complete the case while the Server is unavailable: outbox must survive.
            eventually(lambda: list((rig.root / 'output').glob('pending-report-*.json')))
            assert rig.worker.poll() is None
            rig.start_server()
            completed = eventually(lambda: rig.succeeded(first))
            assert completed['run_id'] == original['run_id']
            assert completed['retry_count'] == 0
            eventually(lambda: not list((rig.root / 'output').glob('pending-report-*.json')))

            # Restart the Server while a case is still running; no second launch.
            second = rig.submit()
            eventually(lambda: rig.executing(second))
            original = rig.task(second)
            rig.server.kill()
            rig.server.wait(timeout=5)
            rig.start_server()
            completed = eventually(lambda: rig.succeeded(second))
            assert completed['run_id'] == original['run_id'] and completed['retry_count'] == 0

            stop(rig.worker)
            rig.start_worker(20)
            third = rig.submit()
            journal_path, journal = eventually(lambda: rig.executing(third))
            old_run = rig.task(third)['run_id']
            rig.worker.kill()
            rig.worker.wait(timeout=5)
            eventually(lambda: Path(journal['receipt']).is_file())
            proof = json.loads(Path(journal['receipt']).read_text())
            assert proof == dict(clean=True, token=Path(journal['receipt']).name)

            # A missing proof must refuse startup, never release/retry the task.
            invalid = dict(journal, receipt=str(rig.root / 'output' / 'missing.json'))
            journal_path.write_text(json.dumps(invalid))
            rig.start_worker(.1)
            assert rig.worker.wait(timeout=5) == 1
            assert rig.task(third)['status'] == 'RUNNING'
            assert rig.task(third)['retry_count'] == 0
            journal_path.write_text(json.dumps(journal))
            rig.start_worker(.1)
            completed = eventually(lambda: rig.succeeded(third))
            assert completed['retry_count'] == 1 and completed['run_id'] != old_run

            stop(rig.worker)
            rig.start_worker(8)
            fourth = rig.submit()
            eventually(lambda: rig.executing(fourth))
            original = rig.task(fourth)
            os.kill(rig.worker.pid, signal.SIGSTOP)
            try:
                eventually(lambda: rig.task(fourth).get('recovery_reason') == 'worker_offline', 8)
                assert rig.task(fourth)['retry_count'] == 0
            finally:
                os.kill(rig.worker.pid, signal.SIGCONT)
            completed = eventually(lambda: rig.succeeded(fourth))
            assert completed['run_id'] == original['run_id'] and completed['retry_count'] == 0

            stop(rig.worker)
            # An old attempt cannot overwrite the new result; recovery ACK replay
            # remains idempotent even after the retry has already succeeded.
            registered = request(rig.port, 'query_status', {})['result']['workers']
            identity = next(w for w in registered if w['worker_id'] == 'recover-slot')
            with socket.create_connection(('127.0.0.1', rig.port), timeout=5) as conn:
                def rpc(kind, data):
                    payload = json.dumps(dict(version=1, type=kind, request_id=77, data=data)).encode()
                    conn.sendall(struct.pack('!I', len(payload)) + payload)
                    def read(size):
                        value = b''
                        while len(value) < size:
                            chunk = conn.recv(size - len(value))
                            if not chunk:
                                raise RuntimeError('truncated response')
                            value += chunk
                        return value
                    length = struct.unpack('!I', read(4))[0]
                    return json.loads(read(length).decode())['data']
                assert rpc('worker_register', dict(worker_id='recover-slot', hostname='test',
                           machine_id=identity['machine_id'], slot_id=identity['slot_id'],
                           slots=1, recovery_version=1))['code'] == 0
                assert rpc('task_result', dict(worker_id='recover-slot', task_id=third,
                           run_id=old_run, status='FAILED', message='{}'))['code'] == 3
                for _ in range(2):
                    assert rpc('worker_recover', dict(worker_id='recover-slot', state='lost',
                        task_id=third, run_id=old_run))['result']['action'] == 'requeued'
                assert rig.task(third)['status'] == 'SUCCEEDED'

            # Restart can replay durable recovery decisions and migrate V1.
            stop(rig.worker)
            stop(rig.server)
            with sqlite3.connect(str(rig.root / 'state.db')) as db:
                rows = db.execute('SELECT task_id, count(*) FROM task_runs GROUP BY task_id').fetchall()
                assert rows == [(first, 1), (second, 1), (third, 2), (fourth, 1)], rows
                assert db.execute('SELECT count(*) FROM recovery_decisions').fetchone()[0] == 1
                db.execute('DROP TABLE recovery_decisions')
                db.execute('PRAGMA user_version=1')
            rig.start_server()
            assert rig.task(third)['status'] == 'SUCCEEDED'
            with sqlite3.connect(str(rig.root / 'state.db')) as db:
                assert db.execute('PRAGMA user_version').fetchone()[0] == 5
                assert db.execute("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='daily_plans'").fetchone()[0] == 1
                assert db.execute('SELECT count(*) FROM task_query_index').fetchone()[0] == 4
        except BaseException:
            for path in sorted(rig.root.glob('process-*.log')):
                print(str(path) + '\n' + path.read_text(errors='replace'), file=sys.stderr)
            raise
        finally:
            rig.close()


if __name__ == '__main__':
    main()
