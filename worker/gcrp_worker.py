#!/usr/bin/env python3
"""Single-slot PJtest executor. Never executes commands from task metadata."""
import argparse
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import uuid
from process_executor import ProcessExecutor
from pjtest_adapter import PJtestAdapter, file_digest
from runtime_config import load_config
from recovery_journal import write_json, remove, sync_directory
from log_retention import prune_worker_logs
from maintenance import SlotMaintenance
from slot_process import publish as publish_process

MAX_FRAME = 1024 * 1024
_SLOT_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}\Z")
_RUN_ID = re.compile(r"[A-Za-z0-9_-]{1,128}\Z")
_LOG_NAMES = ("stdout.log", "stderr.log", "result.env", "result.json",
              "flow_config", "evidence.run", "runner.log", "case-run.log", "evidence.json")
_LOG_CHUNK = 16384
_LOG_LIMIT = 1024 * 1024 * 1024


def result_summary(result):
    """Wire DTO only. Full paths/payload/argv stay in archived result.json."""
    summary = dict(status=result['status'])
    for key in ('reason', 'case_status', 'case_reason'):
        if isinstance(result.get(key), str):
            summary[key] = result[key].encode('utf-8')[:256].decode('utf-8', 'ignore')
    for key in ('case_ret_code', 'raw_exit_code', 'exit_code', 'pid'):
        value = result.get(key)
        if key == 'case_ret_code' and isinstance(value, str):
            try:
                value = int(value)
            except ValueError:
                continue
        if type(value) is int and -1000000000 <= value <= 1000000000:
            summary[key] = value
    return summary


class TransportError(ConnectionError):
    """Only network failures may enter the automatic reconnect path."""


def shared_pjtest_settings(config, worker_id, output):
    """Translate the common config into one dedicated SVN-backed slot."""
    artifact = config.get("worker.artifact_root", "")
    if not artifact:
        return None
    if not _SLOT_ID.fullmatch(worker_id):
        raise ValueError("PJtest worker-id must be 1-64 letters, digits, _ or -")
    slots_root = Path(config.get("worker.slots_root") or "./worker/workers_slots").expanduser().resolve()
    slot = slots_root / worker_id / "galaxcore"
    grace = float(config.get("worker.terminate_grace_seconds") or "30")
    clean_value = config.get("worker.clean") or "true"
    if clean_value not in ("true", "false"):
        raise ValueError("worker.clean must be true or false")
    return dict(galaxcore_root=str(slot), svn_url=config.get("worker.svn_url", ""),
                svn_revision=config.get("worker.svn_revision") or None,
                artifact_root=artifact, log_root=str(output), flow_profiles={},
                require_server_flow=True,
                checkout_lock=str(slots_root / ".checkout.lock"),
                clean=clean_value == "true", terminate_grace_seconds=grace)


class Worker:
    def __init__(self, args):
        self.args = args
        self.sock = None
        self.buffer = bytearray()
        self.sequence = 0
        self.assigned = None
        self.process = None
        self.next_reconnect = 0
        self.next_log_retry = 0
        self.archive_lifecycle = False
        self.retention_days = getattr(args, "retention_days", 30)
        self.next_log_prune = 0
        self.draining = False
        self.maintenance = None
        self.executor = ProcessExecutor(self.heartbeat)
        self.executor.before_launch = self.before_launch
        self.adapter = None
        if args.pjtest_config and not args.pjtest_paths.get("log_root"):
            settings = (args.pjtest_config if isinstance(args.pjtest_config, dict) else
                        json.loads(args.pjtest_config.read_text(encoding="utf-8")))
            args.output = Path(settings["log_root"]).expanduser().resolve()

    def active_path(self):
        return self.pending_path().with_name(self.pending_path().name.replace("pending-report-", "active-"))

    def drain_path(self):
        return self.args.output / ".drain"

    def acknowledge_drain(self):
        if self.drain_path().exists() and not self.draining and self.sock is not None:
            self.request("worker_drain", dict(worker_id=self.args.worker_id))
            self.draining = True
            print("[WORKER] slot draining; current case will finish", flush=True)

    def before_launch(self):
        receipt = self.args.output / ("clean-" + uuid.uuid4().hex + ".json")
        journal = json.loads(self.active_path().read_text(encoding="utf-8"))
        journal["receipt"] = str(receipt.resolve())
        write_json(self.active_path(), journal)
        return receipt

    def disconnect(self):
        if self.sock is not None:
            self.sock.close()
        self.sock = None
        self.buffer.clear()
        self.next_reconnect = time.monotonic() + self.args.reconnect_seconds

    def heartbeat(self):
        # Transport loss must not unwind ProcessExecutor and kill a live case.
        try:
            if self.sock is None:
                if time.monotonic() < self.next_reconnect:
                    return
                self.connect()
            self.acknowledge_drain()
            data = dict(worker_id=self.args.worker_id)
            if self.maintenance and self.maintenance.acknowledgement() is not None:
                data['maintenance_ack'] = self.maintenance.acknowledgement()
            management = self.request("worker_heartbeat", data)
            if self.maintenance and isinstance(management, dict):
                self.maintenance.offer(management.get('command'))
            if self.assigned is None and time.monotonic() >= self.next_log_retry:
                self.next_log_retry = time.monotonic() + 15
                self.try_flush_logs()
            if self.assigned is None and time.monotonic() >= self.next_log_prune:
                self.next_log_prune = time.monotonic() + 3600
                try:
                    prune_worker_logs(self.args.output, self.retention_days)
                    if self.maintenance:
                        self.maintenance.prune_logs(self.retention_days)
                except (OSError, ValueError, RuntimeError) as error:
                    print("[WORKER] log retention deferred: " + str(error), flush=True)
        except TransportError as error:
            if self.sock is not None:
                print("[WORKER] connection lost; execution continues: " + str(error), flush=True)
            self.disconnect()

    def connect(self):
        try:
            self.sock = socket.create_connection((self.args.host, self.args.port), timeout=5)
        except OSError as error:
            raise TransportError(str(error)) from error
        self.buffer.clear()
        registration = self.request("worker_register", dict(worker_id=self.args.worker_id,
                     hostname=socket.gethostname(), machine_id=self.args.machine_id,
                     slot_id=self.args.slot_id, slots=1, recovery_version=1, control_version=1))
        self.archive_lifecycle = isinstance(registration, dict) and registration.get('archive_lifecycle_v1') is True
        self.draining = False
        self.acknowledge_drain()
        self.flush_report()
        data = dict(worker_id=self.args.worker_id, state="idle")
        if self.assigned is not None:
            data.update(state="active", task_id=self.assigned["task_id"],
                        run_id=self.assigned.get("run_id", ""))
        elif self.active_path().exists():
            journal = json.loads(self.active_path().read_text(encoding="utf-8"))
            if (journal.get("worker_id") != self.args.worker_id or
                    type(journal.get("task_id")) is not int or
                    not isinstance(journal.get("run_id"), str) or not journal["run_id"] or
                    "receipt" not in journal):
                raise RuntimeError("invalid active journal; inspect " + str(self.active_path()))
            receipt = journal["receipt"]
            if receipt is not None:
                path = Path(receipt)
                if (path.parent.resolve() != self.args.output.resolve() or
                        path.is_symlink() or not path.is_file() or
                        json.loads(path.read_text(encoding="utf-8")) != dict(clean=True, token=path.name)):
                    raise RuntimeError("execution exit unproven; Worker quarantined: " + str(path))
            data.update(state="lost", task_id=journal["task_id"], run_id=journal["run_id"])
        result = self.request("worker_recover", data)
        action = result.get("action")
        if action in ("requeued", "failed", "cancelled") and data["state"] == "lost":
            remove(self.active_path())
            result = self.request("worker_recover", dict(worker_id=self.args.worker_id, state="idle"))
            action = result.get("action")
        if action not in ("ready", "resume"):
            raise RuntimeError("recovery blocked; inspect server task and local journal: " + repr(result))
        print("[WORKER] connected; recovery=" + action, flush=True)
        self.try_flush_logs()

    def pending_path(self):
        identity = hashlib.sha256(self.args.worker_id.encode("utf-8")).hexdigest()[:24]
        return self.args.output / ("pending-report-" + identity + ".json")

    def save_report(self, report):
        path = self.pending_path()
        fd, temporary = tempfile.mkstemp(prefix=".pending-", dir=str(self.args.output))
        try:
            with os.fdopen(fd, "w") as stream:
                json.dump(report, stream)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, str(path))
            directory_fd = os.open(str(self.args.output), os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)

    def flush_report(self):
        path = self.pending_path()
        if not path.exists():
            return
        report = json.loads(path.read_text(encoding="utf-8"))
        archive = report.pop("_archive", None)
        if (not isinstance(report, dict) or report.get("worker_id") != self.args.worker_id or
                type(report.get("task_id")) is not int or
                report.get("status") not in ("SUCCEEDED", "FAILED", "TIMEOUT") or
                not isinstance(report.get("message"), str)):
            raise RuntimeError("invalid pending report; inspect " + str(path))
        if archive is not None:
            try:
                directory = Path(archive["directory"])
                if (directory.is_symlink() or not directory.is_dir() or
                        self.args.output.resolve() not in directory.resolve().parents):
                    raise RuntimeError("invalid archive directory in pending report")
                self.queue_logs(archive["assignment"], directory)
            except OSError as error:
                # The durable result outbox protects both the files and retry
                # intent. Never acknowledge completion without an upload intent.
                raise TransportError("archive queue pending: " + str(error)) from error
        self.request("task_result", report)
        # The outbox is authoritative if a crash leaves both files behind.
        remove(self.active_path())
        path.unlink()
        directory_fd = os.open(str(self.args.output), os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)

    def queue_logs(self, assignment, directory):
        """Keep an fsynced local upload manifest until every file is acknowledged."""
        run_id = assignment.get("run_id", "")
        if not _RUN_ID.fullmatch(run_id):
            return
        files = []
        for name in _LOG_NAMES:
            source = directory / "evidence" / "run" if name == "evidence.run" else directory / name
            if source.is_file() and not source.is_symlink():
                files.append(name)
                if source.stat().st_size > _LOG_LIMIT:
                    print("[WORKER] log exceeds archive limit; preserve ALL local task logs: " + name, flush=True)
        if not files:
            return
        path = self.args.output / ("pending-logs-" + run_id + ".json")
        if path.is_symlink():
            raise RuntimeError("unsafe log outbox: " + str(path))
        if path.exists():
            previous = json.loads(path.read_text(encoding="utf-8"))
            if (previous.get("task_id") != assignment["task_id"] or
                    previous.get("run_id") != run_id or previous.get("worker_id") != self.args.worker_id or
                    previous.get("directory") != str(directory.resolve()) or
                    not isinstance(previous.get('files'), list) or any(name not in _LOG_NAMES for name in previous['files'])):
                raise RuntimeError("conflicting log outbox: " + str(path))
            if previous['files'] != files:
                if previous.get('cleanup_approved') is True:
                    raise RuntimeError('verified archive file set changed; inspect before cleanup')
                # An old Worker may have omitted an oversized file. Preserve it
                # as an explicit pending obligation rather than lose the result
                # replay or silently authorize partial local cleanup.
                previous['files'] = [name for name in _LOG_NAMES if name in files or name in previous['files']]
                write_json(path, previous)
            return
        write_json(path, dict(task_id=assignment["task_id"], run_id=run_id,
                              worker_id=self.args.worker_id, directory=str(directory.resolve()),
                              files=files))

    def flush_logs(self):
        for path in sorted(self.args.output.glob("pending-logs-*.json")):
            if path.is_symlink():
                raise RuntimeError("unsafe log outbox: " + str(path))
            entry = json.loads(path.read_text(encoding="utf-8"))
            run_id = entry.get("run_id")
            directory = Path(entry.get("directory", ""))
            if (not isinstance(run_id, str) or not _RUN_ID.fullmatch(run_id) or
                    path.name != "pending-logs-" + run_id + ".json" or
                    entry.get("worker_id") != self.args.worker_id or
                    type(entry.get("task_id")) is not int or entry["task_id"] <= 0 or
                    not directory.is_dir() or directory.is_symlink() or
                    self.args.output.resolve() not in directory.resolve().parents or
                    not isinstance(entry.get("files"), list) or
                    any(name not in _LOG_NAMES for name in entry["files"])):
                raise RuntimeError("invalid log outbox: " + str(path))
            if entry.get('cleanup_approved') is True:
                self.cleanup_archived_logs(entry)
                self.report_log_state(entry, 'LOCAL_CLEANED')
                remove(path)
                continue
            self.report_log_state(entry, 'UPLOADING')
            for name in entry["files"]:
                source = self.log_source(directory, name)
                if source.is_symlink() or not source.is_file() or source.stat().st_size > _LOG_LIMIT:
                    raise RuntimeError("log source changed: " + str(source))
                size = source.stat().st_size
                digest = file_digest(source)
                offset = 0
                heartbeat_at = time.monotonic()
                with source.open("rb") as stream:
                    while True:
                        stream.seek(offset)
                        block = stream.read(_LOG_CHUNK)
                        complete = offset + len(block) == size
                        response = self.request("log_append", dict(
                            task_id=entry["task_id"], run_id=run_id,
                            worker_id=self.args.worker_id, name=name,
                            offset=offset, data_hex=block.hex(), complete=complete))
                        next_offset = response.get("next_offset")
                        if type(next_offset) is not int or not 0 <= next_offset <= size:
                            raise RuntimeError("invalid log upload offset")
                        if response.get("complete") is True:
                            if next_offset != size:
                                raise RuntimeError("server log length differs from local file")
                            break
                        if next_offset == offset and not block:
                            raise RuntimeError("log upload made no progress")
                        offset = next_offset
                        if time.monotonic() - heartbeat_at >= 10:
                            self.request("worker_heartbeat", dict(worker_id=self.args.worker_id))
                            heartbeat_at = time.monotonic()
                verified = self.request('log_append', dict(task_id=entry['task_id'], run_id=run_id,
                    worker_id=self.args.worker_id, name=name, offset=size, data_hex='', complete=True,
                    sha256=digest))
                if (verified.get('complete') is not True or verified.get('next_offset') != size or
                        verified.get('verified_sha256') != digest):
                    raise RuntimeError('Server did not verify archive; local logs preserved')
                entry.setdefault('verified_files', {})[name] = dict(size=size, sha256=digest)
            # Persist the Server's verification receipts before deleting anything.
            # A restart after a partial local cleanup resumes here idempotently.
            entry['cleanup_approved'] = True
            write_json(path, entry)
            self.cleanup_archived_logs(entry)
            self.report_log_state(entry, 'LOCAL_CLEANED')
            remove(path)

    def report_log_state(self, entry, phase):
        if not self.archive_lifecycle:
            return
        result = self.request('log_state', dict(task_id=entry['task_id'], run_id=entry['run_id'],
            worker_id=self.args.worker_id, state=dict(phase=phase, files=entry['files'])))
        if result.get('recorded') is not True:
            raise RuntimeError('Server did not persist archive lifecycle state')

    @staticmethod
    def log_source(directory, name):
        source = directory / 'evidence' / 'run' if name == 'evidence.run' else directory / name
        if (name not in _LOG_NAMES or directory.is_symlink() or source.parent.is_symlink() or
                source.is_symlink() or directory.resolve() not in source.resolve().parents):
            raise RuntimeError('unsafe archive source: ' + str(source))
        return source

    def cleanup_archived_logs(self, entry):
        directory = Path(entry['directory'])
        verified = entry.get('verified_files', {})
        if (directory.is_symlink() or not directory.is_dir() or
                self.args.output.resolve() not in directory.resolve().parents or
                not isinstance(verified, dict) or set(verified) != set(entry['files'])):
            raise RuntimeError('invalid archive cleanup receipt; local files preserved')
        # Validate the whole set before the first unlink. Never touch unknown
        # files, ZIPs, SVN copies, journals, or sources changed since upload.
        for name in entry['files']:
            source = self.log_source(directory, name)
            receipt = verified[name]
            if (not isinstance(receipt, dict) or type(receipt.get('size')) is not int or
                    not isinstance(receipt.get('sha256'), str) or
                    not re.fullmatch(r'[0-9a-f]{64}', receipt['sha256'])):
                raise RuntimeError('invalid verified file receipt')
            if source.exists() and (not source.is_file() or source.stat().st_size != receipt['size'] or
                                    file_digest(source) != receipt['sha256']):
                raise RuntimeError('local log changed after upload; preserve ' + str(source))
        write_json(directory / 'archived.json', dict(task_id=entry['task_id'], run_id=entry['run_id'],
            worker_id=entry['worker_id'], verified_files=verified, server_verified=True))
        for name in entry['files']:
            source = self.log_source(directory, name)
            if source.exists():
                source.unlink()
                sync_directory(source.parent)
        evidence = directory / 'evidence'
        if evidence.is_dir() and not evidence.is_symlink() and not any(evidence.iterdir()):
            evidence.rmdir()
            sync_directory(directory)
        print('[WORKER] logs archived and SHA-256 verified; local task logs removed: ' + entry['run_id'], flush=True)

    def try_flush_logs(self):
        try:
            if self.sock is not None:
                self.flush_logs()
        except TransportError as error:
            print("[WORKER] log upload deferred: " + str(error), flush=True)
            self.disconnect()
        except (RuntimeError, OSError, ValueError, TypeError) as error:
            print("[WORKER] log archive needs inspection: " + str(error), flush=True)

    def receive(self, deadline):
        while True:
            if len(self.buffer) >= 4:
                size = struct.unpack("!I", self.buffer[:4])[0]
                if not 0 < size <= MAX_FRAME:
                    raise RuntimeError("invalid frame size")
                if len(self.buffer) >= size + 4:
                    value = json.loads(self.buffer[4:4 + size])
                    del self.buffer[:4 + size]
                    if (not isinstance(value, dict) or type(value.get("version")) is not int
                            or value["version"] != 1 or type(value.get("request_id")) is not int
                            or not 0 <= value["request_id"] < 2**64
                            or not isinstance(value.get("data"), dict)):
                        raise RuntimeError("invalid envelope")
                    return value
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("server response timeout")
            if not select.select([self.sock], [], [], remaining)[0]:
                raise TimeoutError("server response timeout")
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("server disconnected")
            self.buffer.extend(chunk)

    def assignment(self, message):
        data = message["data"]
        if (message.get("type") != "task_assign" or self.assigned is not None
                or type(data.get("task_id")) is not int or not 0 < data["task_id"] < 2**64
                or data.get("task_type") != "REGRESSION"
                or not isinstance(data.get("target"), str)
                or not isinstance(data.get("revision"), str)):
            raise RuntimeError("unexpected or excess assignment")
        self.assigned = data
        if not self.adapter:
            raise RuntimeError("PJtest adapter is not ready")

    def request(self, kind, data):
        try:
            return self._request(kind, data)
        except OSError as error:
            raise TransportError(str(error)) from error

    def _request(self, kind, data):
        self.sequence += 1
        request_id = self.sequence
        body = json.dumps(dict(version=1, type=kind, request_id=request_id,
                               data=data)).encode()
        self.sock.sendall(struct.pack("!I", len(body)) + body)
        verifying = kind == 'log_append' and 'sha256' in data
        deadline = time.monotonic() + (300 if verifying else 5)
        heartbeat_at = time.monotonic() + 1
        heartbeat_ids = set()
        answer = None
        while True:
            if verifying and answer is None and time.monotonic() >= heartbeat_at:
                self.sequence += 1
                heartbeat_ids.add(self.sequence)
                heartbeat = json.dumps(dict(version=1, type='worker_heartbeat', request_id=self.sequence,
                    data=dict(worker_id=self.args.worker_id))).encode()
                self.sock.sendall(struct.pack('!I', len(heartbeat)) + heartbeat)
                heartbeat_at = time.monotonic() + 1
            try:
                message = self.receive(min(deadline, heartbeat_at) if verifying and answer is None else deadline)
            except TimeoutError:
                if verifying and answer is None and time.monotonic() < deadline:
                    continue
                raise
            if message.get("type") == "task_assign":
                self.assignment(message)
                continue
            if (message.get("type") != "response" or
                    type(message["data"].get("code")) is not int):
                raise RuntimeError("request rejected: " + repr(message)[:500])
            if message['request_id'] in heartbeat_ids:
                heartbeat_ids.remove(message['request_id'])
                if message['data']['code'] != 0:
                    raise ConnectionError('verification heartbeat rejected')
                if answer is not None and not heartbeat_ids:
                    return answer.get('result')
                continue
            if message['request_id'] != request_id:
                raise RuntimeError('unexpected response identity')
            if message["data"]["code"] == 4:
                raise ConnectionError("server temporarily unable to persist request")
            if message["data"]["code"] != 0:
                raise RuntimeError("request rejected: " + repr(message)[:500])
            answer = message['data']
            if not heartbeat_ids:
                return answer.get('result')

    def stop_process(self):
        self.executor.stop()
        if self.process is not None:
            # A separate process group prevents orphan descendants on timeout/shutdown.
            try:
                os.killpg(self.process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.process.wait()
            self.process = None

    def execute(self, root):
        assignment = self.assigned
        task_id = assignment["task_id"]
        write_json(self.active_path(), dict(worker_id=self.args.worker_id, task_id=task_id,
                   run_id=assignment.get("run_id", ""), receipt=None))
        start = dict(task_id=task_id, worker_id=self.args.worker_id)
        if assignment.get("run_id"):
            start["run_id"] = assignment["run_id"]
        self.request("task_start", start)
        directory = root / str(task_id)
        if directory.exists():
            directory = Path(tempfile.mkdtemp(prefix="run-", dir=str(self.args.output))) / str(task_id)
        if self.adapter:
            result = self.adapter.execute(assignment, directory)
            if not self.adapter.healthy:
                # Fail closed: no terminal report/released slot on cleanup failure.
                raise RuntimeError("PJtest cleanup failed; Worker quarantined; inspect local result.json")
        else:
            raise RuntimeError("PJtest adapter is not ready")
        status = result["status"]
        self.acknowledge_drain()
        report = dict(task_id=task_id, worker_id=self.args.worker_id,
                      status=status, message=json.dumps(result_summary(result)))
        if assignment.get("run_id"):
            report["run_id"] = assignment["run_id"]
        report["_archive"] = dict(assignment=assignment, directory=str(directory.resolve()))
        self.save_report(report)
        # The server can send the next assignment before acknowledging this result.
        self.assigned = None
        if self.sock is not None:
            self.flush_report()
            self.try_flush_logs()
        print(json.dumps(result), flush=True)

    def run(self):
        self.args.output.mkdir(parents=True, exist_ok=True)
        if self.pending_path().exists() and self.pending_path().is_symlink():
            raise RuntimeError("unsafe pending report path")
        if self.active_path().is_symlink():
            raise RuntimeError("unsafe active journal path")
        lock_fd = os.open(str(self.active_path()) + ".lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        try:
            fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            os.close(lock_fd)
            raise RuntimeError("another Worker owns this identity/output directory")
        root = Path(tempfile.mkdtemp(prefix="run-", dir=str(self.args.output)))
        try:
            publish_process(self.args.output, self.args.worker_id, __file__)
            self.maintenance = SlotMaintenance(self.args.output, self.heartbeat)
            # Validate local exit evidence before touching a possibly live slot.
            if self.active_path().exists() and not self.pending_path().exists():
                journal = json.loads(self.active_path().read_text(encoding="utf-8"))
                receipt = journal.get("receipt")
                if receipt and not Path(receipt).is_file():
                    raise RuntimeError("execution exit unproven; Worker quarantined: " + receipt)
            self.adapter = (PJtestAdapter(self.args.pjtest_config, self.args.worker_id,
                            self.heartbeat, self.args.pjtest_paths) if self.args.pjtest_config else None)
            if self.adapter:
                self.adapter.executor.before_launch = self.before_launch
            while True:
                try:
                    # A disconnected, idle slot may drain locally. There is no
                    # in-flight assignment or unsent result to reconcile.
                    if (self.sock is None and self.drain_path().exists() and
                            self.assigned is None and not self.pending_path().exists()):
                        self.draining = True
                        print("[WORKER] idle slot drained while disconnected", flush=True)
                        return
                    if self.sock is None:
                        time.sleep(max(0, self.next_reconnect - time.monotonic()))
                        self.connect()
                    self.acknowledge_drain()
                    if self.pending_path().exists():
                        self.flush_report()
                    if self.draining and self.assigned is None and not self.pending_path().exists():
                        return
                    if self.assigned is None and not self.pending_path().exists():
                        if self.maintenance.run_if_idle(self.adapter):
                            self.heartbeat()  # Only after update process exit and slot lock release.
                            continue
                    if self.assigned is not None:
                        self.execute(root)
                    else:
                        try:
                            incoming = self.receive(time.monotonic() + 1)
                        except TimeoutError:
                            self.heartbeat()
                            continue
                        except OSError as error:
                            raise TransportError(str(error)) from error
                        self.assignment(incoming)
                except TransportError as error:
                    print("[WORKER] reconnect pending: " + str(error), flush=True)
                    self.disconnect()
        finally:
            try:
                self.stop_process()
                if self.sock is not None:
                    self.sock.close()
            finally:
                os.close(lock_fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", help="override network.server_ip")
    parser.add_argument("--port", type=int, help="override server.port")
    parser.add_argument("--worker-id", required=True)
    parser.add_argument("--machine-id", help="stable host identity for multi-slot manager")
    parser.add_argument("--slot-id", help="stable slot identity on this host")
    parser.add_argument("--output", type=Path, help="override worker.output_dir")
    parser.add_argument("--task-timeout", type=float, default=30)
    parser.add_argument("--pjtest-config", type=Path,
                        help="enable single-case PJtest mode using this local JSON config")
    args = parser.parse_args()
    try:
        config = load_config("worker")
        explicit_output = args.output is not None
        args.host = args.host or config["network.server_ip"]
        args.port = args.port if args.port is not None else int(config["server.port"])
        args.output = args.output or Path(config["worker.output_dir"])
        default_machine = "host-" + re.sub(r"[^A-Za-z0-9_-]", "_", socket.gethostname())[:59]
        args.machine_id = args.machine_id or config.get("worker.machine_id") or default_machine
        args.slot_id = args.slot_id or args.worker_id
        if (not _SLOT_ID.fullmatch(args.machine_id) or
                not _SLOT_ID.fullmatch(args.slot_id)):
            raise ValueError("machine and slot IDs must be 1-64 letters, digits, _ or -")
        args.reconnect_seconds = float(config.get("worker.reconnect_seconds", "2"))
        if not math.isfinite(args.reconnect_seconds) or not 0.05 <= args.reconnect_seconds <= 60:
            raise ValueError("worker.reconnect_seconds must be between 0.05 and 60")
        args.retention_days = int(config.get("log.retention_days", "30"))
        if not 1 <= args.retention_days <= 3650:
            raise ValueError("log.retention_days must be between 1 and 3650")
        explicit_config = args.pjtest_config is not None
        if not explicit_config:
            args.pjtest_config = shared_pjtest_settings(config, args.worker_id, args.output)
        args.pjtest_paths = {}
        if explicit_config:
            if config.get("worker.test2_root"):
                args.pjtest_paths["test2_root"] = config["worker.test2_root"]
            for key in ("galaxcore_root", "svn_url", "svn_revision"):
                if config.get("worker." + key):
                    args.pjtest_paths[key] = config["worker." + key]
            if config.get("worker.artifact_root"):
                args.pjtest_paths["artifact_root"] = config["worker.artifact_root"]
        if explicit_output and explicit_config:
            args.pjtest_paths["log_root"] = str(args.output)
    except (OSError, ValueError, KeyError) as error:
        parser.error("invalid shared config: " + str(error))
    if not args.pjtest_config:
        parser.error("configure worker.artifact_root and worker.svn_url, or provide --pjtest-config")
    if (not 1 <= args.port <= 65535 or not 1 <= len(args.worker_id.encode()) <= 256
            or not math.isfinite(args.task_timeout) or args.task_timeout <= 0):
        parser.error("invalid port, worker identity or duration")
    def terminate(signum, frame):
        raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, terminate)
    try:
        Worker(args).run()
    except KeyboardInterrupt:
        return 0
    except (OSError, ValueError, RuntimeError) as error:
        print("[WORKER] " + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
