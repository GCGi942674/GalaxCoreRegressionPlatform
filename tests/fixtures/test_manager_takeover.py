"""A Manager SIGKILL must not leave surviving slots outside stop/shrink control."""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'worker'))
from slot_process import start_ticks

def wait(predicate):
    end=time.monotonic()+15
    while time.monotonic()<end:
        if predicate():return
        time.sleep(.05)
    raise AssertionError('manager takeover condition timed out')

with tempfile.TemporaryDirectory(prefix='gcrp-takeover-') as folder:
    base=Path(folder);config=base/'worker.conf'
    with socket.socket() as probe:
        probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
    config.write_text('network.server_ip=127.0.0.1\nserver.port={}\nworker.slots=2\n'
        'worker.output_root={}\nworker.slots_root={}\nworker.reconnect_seconds=0.1\n'.format(port,base/'output',base/'slots'))
    env=dict(os.environ,GCRP_CONFIG=str(config),GCRP_MACHINE_NAME='takeover-fixture')
    command=[sys.executable,'-B','-u',str(ROOT/'worker/slot_manager.py'),'--demo']
    manager=None;worker_pid=None;second_pid=None
    with (base/'console.log').open('w') as log:
        try:
            manager=subprocess.Popen(command,cwd=str(ROOT),env=env,stdout=log,stderr=subprocess.STDOUT)
            record=base/'output/takeover-fixture/slot1/.worker-process.json'
            wait(record.exists);worker_pid=json.loads(record.read_text())['pid']
            second_record=base/'output/takeover-fixture/slot2/.worker-process.json'
            wait(second_record.exists);second_pid=json.loads(second_record.read_text())['pid']
            worker_ticks=start_ticks(worker_pid)
            manager.kill();manager.wait()
            assert start_ticks(worker_pid)==worker_ticks
            config.write_text(config.read_text().replace('worker.slots=2','worker.slots=1'))
            manager=subprocess.Popen(command,cwd=str(ROOT),env=env,stdout=log,stderr=subprocess.STDOUT)
            wait(lambda:'adopted slot1' in (base/'console.log').read_text())
            assert start_ticks(worker_pid)==worker_ticks,'takeover restarted a live Worker'
            wait(lambda:start_ticks(second_pid) is None)
            manager.terminate();manager.wait(timeout=15)
            assert manager.returncode==0,(base/'console.log').read_text()
            wait(lambda:start_ticks(worker_pid) is None)
        finally:
            if manager is not None and manager.poll() is None:
                manager.terminate()
                try:manager.wait(timeout=15)
                except subprocess.TimeoutExpired:manager.kill();manager.wait()
            if worker_pid and start_ticks(worker_pid) is not None:
                os.kill(worker_pid,signal.SIGTERM)
                wait(lambda:start_ticks(worker_pid) is None)
            if second_pid and start_ticks(second_pid) is not None:
                os.kill(second_pid,signal.SIGTERM)
                wait(lambda:start_ticks(second_pid) is None)
print('MANAGER_TAKEOVER PASS')
