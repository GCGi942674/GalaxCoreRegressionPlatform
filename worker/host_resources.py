"""Host-wide Linux sampling and bounded snapshot reads (Python 3.6)."""
import json
import math
import os
from pathlib import Path
import re
import sys
import time
import uuid

from recovery_journal import write_json


def small_read(path, limit=65536):
    with open(str(path), 'r') as stream:
        value = stream.read(limit + 1)
    if len(value) > limit:
        raise ValueError('resource input exceeds limit')
    return value


class FailureLog:
    def __init__(self):
        self.next_log = 0

    def failure(self, error):
        now = time.monotonic()
        if now >= self.next_log:
            self.next_log = now + 60
            print('[RESOURCES] unavailable: ' + str(error)[:160], file=sys.stderr, flush=True)


def pressure(name):
    try:
        for line in small_read('/proc/pressure/' + name, 4096).splitlines():
            if line.startswith('some '):
                value = float(dict(item.split('=', 1) for item in line.split()[1:])['avg60'])
                if math.isfinite(value) and 0 <= value <= 100:
                    return value
    except (OSError, ValueError, KeyError):
        pass
    return None


def disk_budget(paths):
    budgets = []
    for path in paths:
        path = Path(path).expanduser().resolve()
        while not path.exists() and path != path.parent:
            path = path.parent
        stat = os.statvfs(str(path))
        budgets.append((stat.f_bavail * stat.f_frsize, stat.f_blocks * stat.f_frsize))
    return min(budgets)


class HostResources(FailureLog):
    def __init__(self, path, machine_id):
        super().__init__()
        self.path = path
        self.machine_id = machine_id
        self.agent_id = uuid.uuid4().hex
        self.sequence = 0
        self.previous = None
        self.next_sample = 0

    def sample(self, slot_count, paths):
        now = time.monotonic()
        self.next_sample = now + 10
        self.sequence += 1
        report = dict(version=1, machine_id=self.machine_id, slot_count=slot_count,
                      agent_id=self.agent_id, sequence=self.sequence,
                      monotonic_seconds=now, valid=False, cpu_percent=None,
                      mem_total_bytes=None, mem_available_bytes=None,
                      swap_in_bytes_per_sec=None, swap_out_bytes_per_sec=None,
                      disk_free_bytes=None, disk_total_bytes=None,
                      psi_memory_some_avg60=None, psi_io_some_avg60=None)
        try:
            # Only the aggregate CPU line is needed; never read all per-core data.
            with open('/proc/stat', 'r') as stream:
                line = stream.readline(4096)
            fields = line.split()
            if fields[0] != 'cpu' or len(fields) < 9:
                raise ValueError('missing aggregate CPU counters')
            cpu = [int(value) for value in fields[1:9]]  # guest already included in user/nice
            mem = {}
            for line in small_read('/proc/meminfo').splitlines():
                key, value = line.split(':', 1)
                mem[key] = int(value.split()[0]) * 1024
            swap = {}
            for line in small_read('/proc/vmstat').splitlines():
                key, value = line.split()
                if key in ('pswpin', 'pswpout'):
                    swap[key] = int(value)
            total, available = mem['MemTotal'], mem['MemAvailable']
            incoming, outgoing = swap['pswpin'], swap['pswpout']
            if min(cpu + [incoming, outgoing, available]) < 0 or not 0 <= available <= total or total <= 0:
                raise ValueError('invalid host counters')
            free, disk_total = disk_budget(paths)
            if free < 0 or disk_total <= 0 or free > disk_total:
                raise ValueError('invalid filesystem counters')
            current = (now, cpu, incoming, outgoing)
            if self.previous is not None:
                before, old_cpu, old_in, old_out = self.previous
                elapsed = now - before
                delta = [a - b for a, b in zip(cpu, old_cpu)]
                if elapsed <= 0 or min(delta + [incoming - old_in, outgoing - old_out]) < 0:
                    raise ValueError('host counters reset')
                ticks = sum(delta)
                if ticks > 0:
                    report['cpu_percent'] = max(0.0, min(100.0,
                        100.0 * (ticks - delta[3] - delta[4]) / ticks))
                page = os.sysconf('SC_PAGE_SIZE')
                report['swap_in_bytes_per_sec'] = (incoming - old_in) * page / elapsed
                report['swap_out_bytes_per_sec'] = (outgoing - old_out) * page / elapsed
            else:
                report['swap_in_bytes_per_sec'] = 0.0
                report['swap_out_bytes_per_sec'] = 0.0
            self.previous = current
            report.update(valid=True, mem_total_bytes=total, mem_available_bytes=available,
                          disk_free_bytes=free, disk_total_bytes=disk_total,
                          psi_memory_some_avg60=pressure('memory'), psi_io_some_avg60=pressure('io'))
        except (OSError, ValueError, KeyError, IndexError) as error:
            self.previous = None
            self.failure(error)
        try:
            write_json(self.path, report)
        except (OSError, ValueError) as error:
            self.failure(error)
            # A failed atomic write cannot be advertised as a fresh sample.
            try:
                os.unlink(str(self.path))
            except OSError:
                pass
        return report


class ResourceSnapshot(FailureLog):
    def __init__(self, machine_id):
        super().__init__()
        self.path = os.environ.get('GCRP_RESOURCE_SNAPSHOT')
        self.machine_id = machine_id
        self.slots = int(os.environ.get('GCRP_MACHINE_SLOTS', '1')) if self.path else 1
        if not 1 <= self.slots <= 128:
            raise ValueError('GCRP_MACHINE_SLOTS must be 1..128')

    def report(self):
        invalid = dict(version=1, machine_id=self.machine_id, slot_count=self.slots, valid=False)
        try:
            sample = json.loads(small_read(self.path, 16384))
            if not isinstance(sample, dict):
                raise ValueError('invalid resource object')
            if isinstance(sample.get('agent_id'), str) and re.fullmatch('[0-9a-f]{32}', sample['agent_id']):
                invalid['agent_id'] = sample['agent_id']
            if type(sample.get('sequence')) is int and sample['sequence'] >= 0:
                invalid['sequence'] = sample['sequence']
            if (type(sample.get('version')) is not int or sample['version'] != 1 or
                    sample.get('machine_id') != self.machine_id or
                    type(sample.get('slot_count')) is not int or not 1 <= sample['slot_count'] <= 128):
                raise ValueError('invalid resource identity')
            self.slots = sample['slot_count']  # retain reloaded count if a later read fails
            invalid['slot_count'] = self.slots
            # An unavailable snapshot needs no sample identity or timestamp.
            if sample.get('valid') is False:
                return invalid
            if 'agent_id' not in invalid or 'sequence' not in invalid:
                raise ValueError('invalid resource sample identity')
            stamp = sample.get('monotonic_seconds')
            if type(stamp) not in (int, float) or not math.isfinite(stamp):
                raise ValueError('invalid resource timestamp')
            age = time.monotonic() - stamp
            if not 0 <= age <= 30 or sample.get('valid') is not True:
                raise ValueError('stale or unavailable resource sample')
            for key in ('cpu_percent', 'mem_total_bytes', 'mem_available_bytes',
                        'swap_in_bytes_per_sec', 'swap_out_bytes_per_sec',
                        'disk_free_bytes', 'disk_total_bytes'):
                value = sample.get(key)
                if value is None and key == 'cpu_percent':
                    continue
                if type(value) not in (int, float) or not math.isfinite(value) or value < 0:
                    raise ValueError('invalid resource metric: ' + key)
            if (sample['cpu_percent'] is not None and sample['cpu_percent'] > 100 or
                    sample['mem_total_bytes'] <= 0 or sample['mem_available_bytes'] > sample['mem_total_bytes'] or
                    sample['disk_total_bytes'] <= 0 or sample['disk_free_bytes'] > sample['disk_total_bytes']):
                raise ValueError('resource metric out of range')
            result = {key: sample[key] for key in (
                'version', 'machine_id', 'slot_count', 'agent_id', 'sequence', 'valid',
                'cpu_percent', 'mem_total_bytes', 'mem_available_bytes',
                'swap_in_bytes_per_sec', 'swap_out_bytes_per_sec', 'disk_free_bytes', 'disk_total_bytes')}
            for key in ('psi_memory_some_avg60', 'psi_io_some_avg60'):
                value = sample.get(key)
                result[key] = value if type(value) in (int, float) and math.isfinite(value) and 0 <= value <= 100 else None
            result['age_ms'] = int(age * 1000)
            return result
        except (OSError, ValueError, TypeError, OverflowError) as error:
            self.failure(error)
            return invalid
