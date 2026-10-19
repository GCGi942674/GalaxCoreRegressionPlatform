#!/usr/bin/env python3
"""Supervise one independent GCRP Worker process per local slot."""
import argparse
import fcntl
import hashlib
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

from recovery_journal import remove, write_json
from runtime_config import load_config
from slot_identity import machine_identity, migrate_legacy_slots, slot_names
from slot_process import ExistingWorker


VALID_ID = re.compile(r'[A-Za-z0-9][A-Za-z0-9_-]{0,63}\Z')


def slot_settings(config):
    try:
        count = int(config.get('worker.slots', ''))
    except ValueError:
        raise ValueError('configure worker.slots as a number from 1 to 128')
    slots = slot_names(count)
    if config.get('worker.checkout_parallelism', '1') != '1':
        raise ValueError('worker.checkout_parallelism currently supports only 1')
    output = Path(config.get('worker.output_root') or './worker-output').expanduser().resolve()
    machine = machine_identity(output)['name']
    if not all(VALID_ID.fullmatch(machine + '-' + slot) for slot in slots):
        raise ValueError('generated Worker ID exceeds 64 characters')
    return machine, slots, output


def identity_lock_path(output, worker_id):
    identity = hashlib.sha256(worker_id.encode('utf-8')).hexdigest()[:24]
    return output / ('active-' + identity + '.json.lock')


class SlotManager:
    def __init__(self, config):
        self.machine, self.desired, self.output_root = slot_settings(config)
        self.slots_root = Path(config.get('worker.slots_root') or './worker/workers_slots').expanduser().resolve()
        self.machine_root = self.output_root / self.machine
        self.machine_root.mkdir(parents=True, exist_ok=True)
        fd = os.open(str(self.machine_root / '.manager.lock'),
                     os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            os.close(fd)
            raise RuntimeError('another manager owns this machine/output root')
        self.lock_fd = fd
        try:
            migrate_legacy_slots(self.slots_root, self.output_root,
                                 self.machine, self.desired)
        except BaseException:
            os.close(fd)
            raise
        self.children = {}
        self.failures = {}
        self.restart_after = {}
        self.stopping = False
        self.reload = False
        # A killed Manager need not kill its Workers. Validate ALL survivors
        # before starting/draining anything; don't pretend they are our children.
        adopted = {}
        try:
            for slot in slot_names(128):
                if self.output(slot).is_dir() and self.running_elsewhere(slot):
                    adopted[slot] = self.existing_worker(slot)
        except BaseException:
            os.close(self.lock_fd)
            raise
        for slot, process in adopted.items():
            self.children[slot] = (process, time.monotonic())
            print('[MANAGER] adopted {} pid={}'.format(slot, process.pid), flush=True)
            if slot not in self.desired:
                self.drain(slot)

    def output(self, slot):
        return self.machine_root / slot

    def worker_id(self, slot):
        return self.machine + '-' + slot

    def drain(self, slot):
        output = self.output(slot)
        output.mkdir(parents=True, exist_ok=True)
        write_json(output / '.drain', dict(machine_id=self.machine, slot_id=slot))
        print('[MANAGER] drain requested: ' + slot, flush=True)

    def running_elsewhere(self, slot):
        output = self.output(slot)
        output.mkdir(parents=True, exist_ok=True)
        path = identity_lock_path(output, self.worker_id(slot))
        fd = os.open(str(path), os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        try:
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return False
            except BlockingIOError:
                return True
        finally:
            os.close(fd)

    def start(self, slot):
        if self.running_elsewhere(slot):
            process = self.existing_worker(slot)
            self.children[slot] = (process, time.monotonic())
            print('[MANAGER] adopted {} pid={}'.format(slot, process.pid), flush=True)
            return
        output = self.output(slot)
        remove(output / '.drain')
        args = [sys.executable, '-B', '-u', str(Path(__file__).with_name('gcrp_worker.py')),
                '--worker-id', self.worker_id(slot), '--machine-id', self.machine,
                '--slot-id', slot, '--output', str(output)]
        process = subprocess.Popen(args, close_fds=True)
        self.children[slot] = (process, time.monotonic())
        print('[MANAGER] started {} pid={}'.format(slot, process.pid), flush=True)

    def existing_worker(self, slot):
        return ExistingWorker(self.output(slot), self.worker_id(slot),
            Path(__file__).with_name('gcrp_worker.py'), lambda: self.running_elsewhere(slot))

    def reload_config(self):
        config = load_config("worker")
        machine, slots, output = slot_settings(config)
        slots_root = Path(config.get('worker.slots_root') or './worker/workers_slots').expanduser().resolve()
        if machine != self.machine or output != self.output_root or slots_root != self.slots_root:
            raise ValueError('machine identity and roots cannot change while running')
        migrate_legacy_slots(self.slots_root, self.output_root, self.machine, slots)
        for slot in self.desired:
            if slot not in slots and slot in self.children:
                self.drain(slot)
        self.desired = slots
        print('[MANAGER] slots now: ' + ','.join(slots), flush=True)

    def run(self):
        try:
            while True:
                if self.reload:
                    self.reload = False
                    try:
                        self.reload_config()
                    except (OSError, ValueError) as error:
                        print('[MANAGER] reload rejected: ' + str(error), file=sys.stderr, flush=True)
                for slot, item in list(self.children.items()):
                    process, started = item
                    result = process.poll()
                    if result is None:
                        continue
                    del self.children[slot]
                    if self.stopping or slot not in self.desired:
                        print('[MANAGER] drained {} exit={}'.format(slot, result), flush=True)
                        continue
                    if time.monotonic() - started >= 30:
                        self.failures[slot] = 0
                    count = self.failures.get(slot, 0) + 1
                    self.failures[slot] = count
                    if count >= 5:
                        print('[MANAGER] slot {} quarantined after repeated exits'.format(slot),
                              file=sys.stderr, flush=True)
                    else:
                        self.restart_after[slot] = time.monotonic() + min(2 ** count, 30)
                        print('[MANAGER] slot {} exited {}; retry {}'.format(slot, result, count),
                              file=sys.stderr, flush=True)
                if self.stopping and not self.children:
                    return
                if not self.stopping:
                    for slot in self.desired:
                        if (slot not in self.children and self.failures.get(slot, 0) < 5 and
                                time.monotonic() >= self.restart_after.get(slot, 0)):
                            self.start(slot)
                time.sleep(.2)
        finally:
            # A manager crash does not revoke the slot process's OS identity lock.
            os.close(self.lock_fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--migration-preview', action='store_true',
                        help='show legacy SVN copies that would be reused, then exit')
    args = parser.parse_args()
    try:
        config = load_config("worker")
        if args.migration_preview:
            machine, slots, output = slot_settings(config)
            root = Path(config.get('worker.slots_root') or './worker/workers_slots').expanduser().resolve()
            migrate_legacy_slots(root, output, machine, slots, preview=True)
            return 0
        manager = SlotManager(config)
    except (OSError, ValueError, RuntimeError) as error:
        parser.error(str(error))

    def terminate(_signum, _frame):
        if not manager.stopping:
            manager.stopping = True
            for slot in list(manager.children):
                manager.drain(slot)

    def reload(_signum, _frame):
        manager.reload = True

    signal.signal(signal.SIGTERM, terminate)
    signal.signal(signal.SIGINT, terminate)
    signal.signal(signal.SIGHUP, reload)
    try:
        manager.run()
    except (OSError, ValueError, RuntimeError) as error:
        print('[MANAGER] ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
