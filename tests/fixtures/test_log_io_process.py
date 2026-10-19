"""Real transport, durable archive, concurrent queries and half-close."""
import json
import os
from pathlib import Path
import socket
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from test_sqlite_worker import wait_server, stop, request
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from backup_database import backup_database

with tempfile.TemporaryDirectory(prefix='gcrp-log-io-') as folder:
    root = Path(folder)
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    config = root / 'config.conf'
    config.write_text('server.bind_ip=127.0.0.1\nnetwork.server_ip=127.0.0.1\n'
        'server.port={}\nserver.io_threads=2\nserver.worker_threads=4\n'
        'log.level=ERROR\nlog.dir={}\ndatabase.path={}\nserver.log_archive_root={}\n'
        'worker.output_dir={}\n'.format(port,root/'logs',root/'state.db',root/'archive',root/'output'))
    env = dict(os.environ, GCRP_CONFIG=str(config))
    server = worker = None
    with (root/'console.log').open('w') as log:
        try:
            server = subprocess.Popen([sys.argv[1]],cwd=str(root),env=env,stdout=log,stderr=subprocess.STDOUT)
            wait_server(port, server)
            worker = subprocess.Popen([sys.executable,'-B',str(Path(__file__).resolve().parents[2]/'worker/gcrp_worker.py'),
                '--worker-id','log-fixture','--demo','--demo-seconds','0.01'],cwd=str(root),env=env,stdout=log,stderr=subprocess.STDOUT)
            ids = [request(port,'submit_task',dict(task_type='REGRESSION',target='logs',revision='demo'))['result']['task_id'] for _ in range(8)]
            deadline = time.monotonic()+25
            while True:
                tasks = [request(port,'query_task',{'task_id':id})['result'] for id in ids]
                if all(task['status']=='SUCCEEDED' for task in tasks):
                    archives=[request(port,'query_logs',dict(task_id=t['task_id'],run_id=t['run_id'])) for t in tasks]
                    if all(len(a['result']['items'])>=3 and all(i['complete'] for i in a['result']['items'])
                           and a['result']['archive_state']=='LOCAL_CLEANED' for a in archives):
                        if len(list((root/'output').glob('run-*/*/archived.json')))==len(ids):
                            break
                assert time.monotonic()<deadline, (tasks, (root/'console.log').read_text()[-2000:])
                time.sleep(.05)
            assert not list((root/'output').glob('run-*/*/stdout.log'))
            assert not list((root/'output').glob('run-*/*/stderr.log'))
            assert not list((root/'output').glob('run-*/*/result.json'))
            archived_stdout=request(port,'read_log',dict(task_id=tasks[0]['task_id'],run_id=tasks[0]['run_id'],name='stdout.log',offset=0,limit=16384))
            assert b'demo stdout' in bytes.fromhex(archived_stdout['result']['data_hex'])
            with ThreadPoolExecutor(max_workers=8) as pool:
                results=list(pool.map(lambda _:request(port,'query_status',{}), range(40)))
            assert all(r['code']==0 and r['result']['total']==8 for r in results)
            body=json.dumps(dict(version=1,type='query_logs',request_id=1,
                data=dict(task_id=tasks[0]['task_id'],run_id=tasks[0]['run_id']))).encode()
            with socket.create_connection(('127.0.0.1',port),timeout=5) as client:
                client.sendall(struct.pack('!I',len(body))+body)
                client.shutdown(socket.SHUT_WR)
                def read(size):
                    data=b''
                    while len(data)<size:
                        chunk=client.recv(size-len(data))
                        assert chunk,'half-close lost asynchronous reply'
                        data+=chunk
                    return data
                reply=json.loads(read(struct.unpack('!I',read(4))[0]).decode())
                assert reply['data']['code']==0 and len(reply['data']['result']['items'])>=3,reply
            # Take a WAL-consistent backup while the real Server is live, then
            # restore a COPY into a different path and boot the actual C++ Server.
            saved=backup_database({'database.path':str(root/'state.db'),'database.min_free_bytes':'0'})
            stop(worker)
            worker=None
            stop(server)
            server=None
            restored=root/'restored.db'
            shutil.copyfile(str(saved),str(restored))
            config.write_text(config.read_text().replace(str(root/'state.db'),str(restored)))
            server=subprocess.Popen([sys.argv[1]],cwd=str(root),env=env,stdout=log,stderr=subprocess.STDOUT)
            wait_server(port,server)
            assert request(port,'query_status',{})['result']['total']==8
            assert all(request(port,'query_task',{'task_id':id})['result']['status']=='SUCCEEDED' for id in ids)
            restored_log=request(port,'read_log',dict(task_id=tasks[0]['task_id'],run_id=tasks[0]['run_id'],name='stdout.log',offset=0,limit=16384))
            assert restored_log['result']['data_hex']==archived_stdout['result']['data_hex']
            new=request(port,'submit_task',dict(task_type='REGRESSION',target='restored',revision='demo'))
            assert new['result']['task_id']==9,new
        finally:
            stop(worker)
            stop(server)
print('LOG_IO_PROCESS PASS')
