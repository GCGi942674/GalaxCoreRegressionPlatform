"""Fail-closed single-case PJtest execution in a dedicated slot checkout."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import subprocess
import tempfile
import time
from timeout_metrics import SCOPE, script_fingerprint, select_timeout
import uuid
import zipfile

from process_executor import ProcessExecutor, ExecutionContainmentError

_SAFE_REVISION = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}\Z")
_SAFE_FLOW = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}\Z")
_RESULT_KEY = re.compile(r"[A-Z][A-Z0-9_]*\Z")
_MAX_RESULT_BYTES = 65536


class AdapterError(RuntimeError):
    pass


def tree_digest(root):
    """Content identity of the execution inputs, excluding generated runtime."""
    root = Path(root)
    digest = hashlib.sha256()

    def visit(directory):
        for path in sorted(directory.iterdir()):
            relative = path.relative_to(root).as_posix()
            if path.name in (".svn", "__pycache__") or relative == "vivado_runner/runtime":
                continue
            if path.is_symlink():
                raise AdapterError("symlink in test2 inputs: " + relative)
            mode = path.stat().st_mode
            if stat.S_ISDIR(mode):
                digest.update(b"D\0" + relative.encode() + b"\0")
                visit(path)
            elif stat.S_ISREG(mode):
                digest.update(b"F\0" + relative.encode() + b"\0")
                digest.update(str(stat.S_IMODE(mode)).encode() + b"\0")
                digest.update(str(path.stat().st_size).encode() + b"\0")
                with path.open("rb") as stream:
                    for block in iter(lambda: stream.read(1024 * 1024), b""):
                        digest.update(block)
            else:
                raise AdapterError("non-regular test2 input: " + relative)

    visit(root)
    return digest.hexdigest()


def file_digest(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


class PJtestAdapter:
    def __init__(self, config_file, worker_id, heartbeat, path_overrides=None):
        config = (
            dict(config_file)
            if isinstance(config_file, dict)
            else json.loads(Path(config_file).read_text(encoding="utf-8"))
        )
        if path_overrides:
            config.update(path_overrides)
        self.galaxcore_root = None
        if config.get("galaxcore_root"):
            self.galaxcore_root = Path(config["galaxcore_root"]).expanduser().resolve()
            self._ensure_checkout(config)
            self.work_root = self.galaxcore_root / "test2"
        else:
            self.work_root = Path(config["test2_root"]).expanduser().resolve(strict=True)
        self.artifacts = Path(config["artifact_root"]).expanduser().resolve(strict=True)
        self.log_root = Path(config["log_root"]).expanduser().resolve()
        self.worker_id = worker_id
        self.profiles = config["flow_profiles"]
        self.require_server_flow = bool(config.get("require_server_flow", False))
        self.clean = config.get("clean", True)
        self.environment = config.get("environment", {})
        if not isinstance(self.environment, dict) or any(
            not isinstance(k, str)
            or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", k)
            or not isinstance(v, str)
            or "\0" in v
            for k, v in self.environment.items()
        ):
            raise AdapterError("invalid local environment")
        self.grace = float(config.get("terminate_grace_seconds", 2))
        if (
            not self.work_root.is_dir()
            or not self.artifacts.is_dir()
            or not isinstance(self.profiles, dict)
            or (not self.profiles and self.galaxcore_root is None and not self.require_server_flow)
            or type(self.clean) is not bool
            or not 0 <= self.grace <= 60
            or not (self.work_root / "run.sh").is_file()
        ):
            raise AdapterError("invalid local PJtest configuration")
        self.log_root.mkdir(parents=True, exist_ok=True)
        self.executor = ProcessExecutor(heartbeat)
        self.healthy = True

    def _ensure_checkout(self, config):
        checkout_lock = config.get("checkout_lock")
        if checkout_lock:
            lock_path = Path(checkout_lock).expanduser().resolve()
            lock_path.parent.mkdir(parents=True, exist_ok=True)
            lock_fd = os.open(str(lock_path), os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
            try:
                fcntl.flock(lock_fd, fcntl.LOCK_EX)
                self._checkout_locked(config)
            finally:
                os.close(lock_fd)
            return
        self._checkout_locked(config)

    def _checkout_locked(self, config):
        root = self.galaxcore_root
        if not root.exists():
            svn_url = config.get("svn_url")
            revision = config.get("svn_revision")
            if not isinstance(svn_url, str) or not svn_url:
                raise AdapterError("slot checkout missing; configure svn_url")
            if revision is not None and (
                not isinstance(revision, str) or not re.fullmatch(r"[0-9]+", revision)
            ):
                raise AdapterError("invalid svn_revision")
            root.parent.mkdir(parents=True, exist_ok=True)
            command = ["svn", "checkout", "--non-interactive"]
            if revision:
                command.extend(["-r", revision])
            command.extend([svn_url, str(root)])
            # A failed checkout is left for the operator to inspect; never
            # delete an existing or partly populated slot automatically.
            try:
                print("[WORKER] SVN checkout: {} -> {}".format(svn_url, root), flush=True)
                subprocess.run(command, check=True, timeout=7200, stdout=subprocess.DEVNULL)
            except (OSError, subprocess.SubprocessError) as error:
                raise AdapterError("slot SVN checkout failed: " + str(error)) from error
        required = (
            (root, "directory"),
            (root / ".svn", "directory"),
            (root / "test2", "directory"),
            (root / "test2/run.sh", "file"),
        )
        missing = [
            str(path)
            for path, kind in required
            if not (path.is_dir() if kind == "directory" else path.is_file())
        ]
        if missing:
            raise AdapterError("slot SVN checkout incomplete; missing: " + ", ".join(missing))

    def update_checkout(self, scope, executor, log):
        """Update only this complete slot, under the same lock as testcase execution."""
        import xml.etree.ElementTree as ET

        if scope not in ("test2", "all") or self.galaxcore_root is None:
            raise AdapterError("SVN update requires a complete GalaxCore checkout")
        root = self.galaxcore_root
        if not (root / ".svn").is_dir() or not (root / "test2/run.sh").is_file():
            raise AdapterError("incomplete SVN slot; update refused")
        fd = os.open(self.work_root, os.O_RDONLY | os.O_DIRECTORY)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            target = root if scope == "all" else self.work_root
            # No revert/cleanup/delete; preserve local modifications and report conflicts.
            _, code, timeout = executor.run(
                ["svn", "update", "--non-interactive", "--", str(target)],
                root,
                os.environ.copy(),
                log,
                log,
                7200,
                grace=self.grace,
            )
            if code or timeout:
                raise AdapterError("SVN update failed or timed out; inspect maintenance log")
            with tempfile.TemporaryFile() as status:
                _, code, timeout = executor.run(
                    ["svn", "status", "--xml", "--", str(target)],
                    root,
                    os.environ.copy(),
                    status,
                    log,
                    120,
                    grace=self.grace,
                )
                if code or timeout:
                    raise AdapterError("SVN conflict check failed")
                status.seek(0)
                contents = status.read(4 * 1024 * 1024 + 1)
                if len(contents) > 4 * 1024 * 1024:
                    raise AdapterError("SVN status exceeds inspection limit")
                tree = ET.fromstring(contents)
                for row in tree.iter("wc-status"):
                    if (
                        row.get("item") in ("conflicted", "obstructed", "missing", "incomplete")
                        or row.get("props") == "conflicted"
                        or row.get("tree-conflicted") == "true"
                    ):
                        raise AdapterError(
                            "SVN working copy has conflicts; manual inspection required"
                        )
            if not (root / "test2/run.sh").is_file():
                raise AdapterError("SVN update removed required test2 entrypoint")
        finally:
            os.close(fd)

    def validate(self, task):
        payload = task.get("payload")
        version = payload.get("spec_version") if isinstance(payload, dict) else None
        required = {"spec_version", "case", "flow", "timeout_seconds"}
        if (
            task.get("task_type") != "REGRESSION"
            or type(task.get("task_id")) is not int
            or task["task_id"] <= 0
            or not isinstance(payload, dict)
            or not required.issubset(payload)
            or type(version) is not int
            or version not in (1, 2)
            or (version == 1 and (set(payload) != required or self.require_server_flow))
            or (
                version == 2
                and (
                    set(payload)
                    not in (required | {"flow_config"}, required | {"flow_config", "context"})
                )
            )
            or type(payload["timeout_seconds"]) is not int
            or not 1 <= payload["timeout_seconds"] <= 86400
        ):
            raise AdapterError("invalid regression payload")
        relative, flow, revision = payload["case"], payload["flow"], task.get("revision")
        if (
            not isinstance(relative, str)
            or len(relative) > 512
            or not re.fullmatch(r"[A-Za-z0-9_.+\-/]+", relative)
            or any(x in ("", ".", "..") for x in relative.split("/"))
            or not relative.endswith("/run.tcl")
            or not isinstance(flow, str)
            or not _SAFE_FLOW.fullmatch(flow)
            or (version == 1 and flow not in self.profiles and self.galaxcore_root is None)
            or not isinstance(revision, str)
            or not _SAFE_REVISION.fullmatch(revision)
        ):
            raise AdapterError("invalid case, flow or revision")
        case = (self.work_root / relative).resolve(strict=True)
        if self.work_root not in case.parents or not case.is_file() or case.name != "run.tcl":
            raise AdapterError("case must be a local run.tcl")
        settings = payload["flow_config"] if version == 2 else self.profiles.get(flow, {})
        if (
            not isinstance(settings, dict)
            or (not settings and (version == 2 or self.galaxcore_root is None))
            or len(settings) > 64
        ):
            raise AdapterError("invalid flow_config")
        if version == 2 and "context" in payload:
            context = payload["context"]
            if (
                not isinstance(context, dict)
                or len(context) > 6
                or any(
                    key not in ("template", "suite", "batch_id", "name", "source", "date")
                    or not isinstance(value, str)
                    or len(value) > 128
                    for key, value in context.items()
                )
            ):
                raise AdapterError("invalid regression context")
        for key, value in settings.items():
            if (
                not isinstance(key, str)
                or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key)
                or len(key) > 64
                or (version == 2 and (type(value) is not int or not 0 <= value <= 1000000))
                or isinstance(value, (dict, list, bool))
                or not isinstance(value, (str, int, float))
                or "\n" in str(value)
                or "\r" in str(value)
            ):
                raise AdapterError("invalid local flow setting")
        return case, settings

    def artifact(self, revision):
        candidates = [
            self.artifacts / name
            for name in (
                f"GalaxCore_{revision}.zip",
                f"Galaxcore_{revision}.zip",
                f"GalaxCore_r{revision}.zip",
                f"GalaxCore-{revision}.zip",
            )
        ]
        found = [p for p in candidates if p.exists() or p.is_symlink()]
        if not found:
            raise AdapterError("build artifact not found for revision " + revision)
        if len(found) != 1 or found[0].is_symlink() or not found[0].is_file():
            raise AdapterError("ambiguous or unsafe artifact")
        return found[0]

    def _prepare(self, artifact, revision, directory):
        if self.galaxcore_root is not None:
            return self._prepare_full_slot(artifact, revision, directory)
        manifest_path = Path(str(artifact) + ".manifest.json")
        if manifest_path.is_symlink():
            raise AdapterError("unsafe artifact manifest")
        with manifest_path.open("rb") as stream:
            raw = stream.read(65537)
        if len(raw) > 65536:
            raise AdapterError("invalid artifact manifest")
        manifest = json.loads(raw)
        if (
            type(manifest.get("spec_version")) is not int
            or manifest["spec_version"] != 1
            or manifest.get("revision") != revision
            or not isinstance(manifest.get("test2_revision"), str)
            or not manifest["test2_revision"]
            or any(
                not isinstance(manifest.get(k), str)
                or not re.fullmatch(r"[0-9a-f]{64}", manifest[k])
                for k in ("archive_sha256", "test2_sha256")
            )
        ):
            raise AdapterError("invalid artifact manifest")
        # Copy first, validate the private bytes. A producer replacing/changing the
        # source cannot change what will subsequently be installed.
        archive = directory / "artifact.zip"
        if artifact.stat().st_size > 2 * 1024**3:
            raise AdapterError("artifact size limit")
        shutil.copyfile(artifact, archive)
        if file_digest(archive) != manifest["archive_sha256"]:
            raise AdapterError("artifact hash mismatch")
        source = self.work_root
        if tree_digest(source) != manifest["test2_sha256"]:
            raise AdapterError("test2 hash mismatch")
        slot = directory / "workspace"
        slot.mkdir()
        destination = slot / "test2"

        def ignore(parent, names):
            excluded = {n for n in names if n in (".svn", "__pycache__")}
            if Path(parent) == source / "vivado_runner":
                excluded.add("runtime")
            return excluded

        shutil.copytree(source, destination, symlinks=True, ignore=ignore)
        if tree_digest(destination) != manifest["test2_sha256"]:
            raise AdapterError("test2 changed during snapshot")
        binary = slot / "bin/Linux_64/GalaxCore"
        flow_count = 0
        seen = set()
        with zipfile.ZipFile(archive) as zf:
            items = zf.infolist()
            if len(items) > 100000 or sum(x.file_size for x in items) > 2 * 1024**3:
                raise AdapterError("artifact extraction limit")
            binaries = [
                x
                for x in items
                if not x.is_dir() and Path(x.filename).name in ("GalaxCore", "Galaxcore")
            ]
            if len(binaries) != 1:
                raise AdapterError("artifact must contain one GalaxCore binary")
            for item in items:
                parts = Path(item.filename).parts
                if (
                    item.filename.startswith("/")
                    or "\\" in item.filename
                    or any(x in ("..", ".") for x in item.filename.split("/"))
                    or stat.S_IFMT(item.external_attr >> 16) == stat.S_IFLNK
                ):
                    raise AdapterError("unsafe artifact archive")
                if item.is_dir():
                    continue
                if item == binaries[0]:
                    target = binary
                elif "flow" in parts:
                    if parts.index("flow") == len(parts) - 1:
                        raise AdapterError("invalid flow artifact entry")
                    target = slot / "flow" / Path(*parts[parts.index("flow") + 1 :])
                    flow_count += 1
                else:
                    continue
                if target in seen:
                    raise AdapterError("duplicate artifact destination")
                seen.add(target)
                target.parent.mkdir(parents=True, exist_ok=True)
                with zf.open(item) as src, target.open("xb") as dst:
                    shutil.copyfileobj(src, dst)
        if not flow_count:
            raise AdapterError("artifact missing flow")
        binary.chmod(0o755)
        (directory / "manifest.json").write_text(json.dumps(manifest, indent=2))
        return destination, binary, manifest

    def _prepare_full_slot(self, artifact, revision, directory):
        """Install only the versioned binary into a complete slot checkout."""
        source = self.work_root
        manifest_path = Path(str(artifact) + ".manifest.json")
        if manifest_path.is_symlink():
            raise AdapterError("unsafe artifact manifest")
        manifest = None
        if manifest_path.exists():
            with manifest_path.open("rb") as stream:
                raw = stream.read(65537)
            if len(raw) > 65536:
                raise AdapterError("invalid artifact manifest")
            manifest = json.loads(raw)
            if (
                not isinstance(manifest, dict)
                or type(manifest.get("spec_version")) is not int
                or manifest["spec_version"] != 1
                or manifest.get("revision") != revision
                or not isinstance(manifest.get("test2_revision"), str)
                or not manifest["test2_revision"]
                or any(
                    not isinstance(manifest.get(key), str)
                    or not re.fullmatch(r"[0-9a-f]{64}", manifest[key])
                    for key in ("archive_sha256", "test2_sha256")
                )
            ):
                raise AdapterError("invalid artifact manifest")
        if artifact.stat().st_size > 2 * 1024**3:
            raise AdapterError("artifact size limit")
        source_hash = file_digest(artifact)
        archive = directory / "artifact.zip"
        shutil.copyfile(artifact, archive)
        if file_digest(archive) != source_hash or (
            manifest and manifest["archive_sha256"] != source_hash
        ):
            raise AdapterError("artifact hash mismatch")
        test2_hash = tree_digest(source) if manifest else None
        if manifest and manifest["test2_sha256"] != test2_hash:
            raise AdapterError("test2 hash mismatch")
        binary = self.galaxcore_root / "bin/Linux_64/GalaxCore"
        for path in (binary.parent.parent, binary.parent, binary):
            if path.is_symlink():
                raise AdapterError("unsafe slot binary path")
        # A previous Worker can leave a live executable after a crash.
        for process in Path("/proc").iterdir():
            if not process.name.isdigit():
                continue
            try:
                executable = os.readlink(str(process / "exe"))
                if executable.endswith(" (deleted)"):
                    executable = executable[:-10]
            except (OSError, PermissionError):
                continue
            if executable == str(binary):
                raise AdapterError("slot_busy: old GalaxCore process still running")
        with zipfile.ZipFile(archive) as package:
            entries = package.infolist()
            if len(entries) > 100000 or sum(entry.file_size for entry in entries) > 2 * 1024**3:
                raise AdapterError("artifact extraction limit")
            binaries = [
                entry
                for entry in entries
                if not entry.is_dir() and Path(entry.filename).name in ("GalaxCore", "Galaxcore")
            ]
            if len(binaries) != 1:
                raise AdapterError("artifact must contain one GalaxCore binary")
            for entry in entries:
                if (
                    entry.filename.startswith("/")
                    or "\\" in entry.filename
                    or any(part in (".", "..") for part in entry.filename.split("/"))
                    or stat.S_IFMT(entry.external_attr >> 16) == stat.S_IFLNK
                ):
                    raise AdapterError("unsafe artifact archive")
            binary.parent.mkdir(parents=True, exist_ok=True)
            temporary = None
            try:
                with tempfile.NamedTemporaryFile(
                    dir=str(binary.parent), prefix=".GalaxCore-", delete=False
                ) as output:
                    temporary = Path(output.name)
                    with package.open(binaries[0]) as input_file:
                        shutil.copyfileobj(input_file, output)
                    output.flush()
                    os.fsync(output.fileno())
                temporary.chmod(0o755)
                os.replace(str(temporary), str(binary))
            finally:
                if temporary is not None and temporary.exists():
                    temporary.unlink()
        observed = dict(
            manifest or {},
            spec_version=1,
            revision=revision,
            archive_sha256=source_hash,
            test2_sha256=test2_hash,
            run_sh_sha256=file_digest(source / "run.sh"),
            provenance="publisher_manifest" if manifest else "worker_observed",
        )
        (directory / "manifest.json").write_text(json.dumps(observed, indent=2))
        return source, binary, observed

    def _run_clean(self, env, output):
        script = self.work_root / "clean.sh"
        if self.clean and script.is_file():
            _, code, timed_out = self.executor.run(
                ["bash", str(script)], self.work_root, env, output, output, 120, self.grace
            )
            return 124 if timed_out else code
        return 0

    @staticmethod
    def _write_flow_config(path, settings):
        lines = path.read_text(encoding="utf-8").splitlines() if path.exists() else []
        entry = re.compile(r"^\s*(?:set\s+)?([A-Za-z_][A-Za-z0-9_]*)(?:\s*=\s*|\s+)")
        lines = [
            line for line in lines if not (entry.match(line) and entry.match(line)[1] in settings)
        ]
        lines.extend(f"{key} {value}" for key, value in sorted(settings.items()))
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    @staticmethod
    def _parse(path, case):
        # O_NOFOLLOW plus a strict byte limit, never eval/source shell output.
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
        with os.fdopen(fd, "rb") as stream:
            if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
                raise AdapterError("invalid_result")
            raw = stream.read(_MAX_RESULT_BYTES + 1)
        if len(raw) > _MAX_RESULT_BYTES:
            raise AdapterError("invalid_result")
        result = {}
        for line in raw.decode("utf-8", "strict").splitlines():
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                raise AdapterError("invalid_result")
            key, value = line.split("=", 1)
            if not _RESULT_KEY.fullmatch(key) or key in result:
                raise AdapterError("invalid_result")
            # Handles ordinary printf %q escaping (including spaces); reject
            # unsupported ANSI-C quoting rather than interpret arbitrary shell.
            values = shlex.split(value)
            if len(values) != 1:
                raise AdapterError("invalid_result")
            result[key] = values[0]
        if result.get("RUN_TCL") != str(case) or result.get("STATUS") not in (
            "PASS",
            "FAIL",
            "FAILED",
            "ERROR",
            "TIMEOUT",
            "INTERRUPTED",
        ):
            raise AdapterError("invalid_result")
        if "RET_CODE" in result:
            try:
                int(result["RET_CODE"])
            except ValueError as error:
                raise AdapterError("invalid_result") from error
        return result

    @staticmethod
    def _status_directories(root, namespace, case):
        """Mirror vivado_runner sanitize_relpath, retaining unmodified-runner compatibility."""
        relative = case.parent.relative_to(root).as_posix()
        sanitized = re.sub(r"[^A-Za-z0-9._/-]", "_", relative)
        status = root / "vivado_runner/runtime/workspaces" / namespace / "status"
        return tuple(dict.fromkeys((status / sanitized, status / relative)))

    def _collect_case_result(self, result, directories, case, output_dir, code, timed_out):
        # ProcessExecutor returns only after the supervisor has proved the tree
        # empty. Never search another namespace or infer PASS from stdout.
        if not self.executor.safe:
            raise ExecutionContainmentError("execution tree has no clean completion proof")
        completion = dict(getattr(self.executor, "last_completion", {}))
        result["supervisor"] = completion
        result["expected_result_envs"] = [str(path / "result.env") for path in directories]
        selected = None
        data = None
        try:
            for directory in directories:
                candidate = directory / "result.env"
                if any(path.is_symlink() for path in (candidate, *candidate.parents)):
                    raise AdapterError("unsafe result path")
                if not candidate.exists():
                    continue
                if selected is not None:
                    raise AdapterError("ambiguous_result")
                data = self._parse(candidate, case)
                selected = candidate
        except (AdapterError, OSError, ValueError) as error:
            if not timed_out:
                raise
            # A proven outer timeout must not become ERROR merely because its
            # interrupted result file is partial, absent or malformed.
            result["result_read_error"] = str(error)[:256]
            data = None
            selected = None
        if data is not None:
            result.update(
                result_env=str(selected),
                case_status="FAIL" if data["STATUS"] == "FAILED" else data["STATUS"],
                case_reason=data.get("REASON"),
                case_ret_code=data.get("RET_CODE"),
            )
            try:
                shutil.copyfile(selected, output_dir / "result.env")
            except OSError as error:
                if not timed_out and data["STATUS"] != "TIMEOUT":
                    raise
                result["result_archive_error"] = str(error)[:256]
        if timed_out:
            result.update(status="TIMEOUT", reason="timeout", exit_code=124)
        elif data is not None and data["STATUS"] == "TIMEOUT":
            result.update(status="TIMEOUT", reason="timeout", exit_code=124)
        elif code == 125 and not (
            data is not None and completion.get("reason") == "descendants_reaped"
        ):
            result.update(status="FAILED", reason="execution_tree_incomplete")
        elif data is None:
            result.update(
                status="FAILED", reason="missing_result" if code == 0 else "process_failed"
            )
        elif data["STATUS"] == "PASS":
            result.update(status="SUCCEEDED", reason="pass", exit_code=0)
        else:
            result.update(status="FAILED", reason="business_failed")
        if code == 125 and data is not None:
            result["execution_warning"] = "supervisor_exit_125:" + completion.get(
                "reason", "unknown"
            )
            if result["status"] in ("SUCCEEDED", "TIMEOUT"):
                result["reason"] += (
                    "_after_descendant_cleanup"
                    if completion.get("reason") == "descendants_reaped"
                    else "_with_supervisor_exit_125"
                )
        return selected

    @staticmethod
    def _run_identity(path):
        if path.is_symlink() or not path.is_file():
            return None
        value = path.stat()
        return value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_ctime_ns

    def _capture_run(self, case, directory, baseline, launched):
        source = case.parent / "run"
        current = self._run_identity(source)
        info = dict(captured=False, complete=False, reason="execution_not_started")
        if launched:
            info["reason"] = "run_not_created_or_changed_this_execution"
            if current is not None and current != baseline:
                evidence = directory / "evidence"
                evidence.mkdir(exist_ok=True)
                digest = hashlib.sha256()
                size = 0
                heartbeat_at = time.monotonic()
                with source.open("rb") as src, (evidence / "run").open("xb") as dst:
                    while True:
                        block = src.read(1024 * 1024)
                        if not block:
                            break
                        dst.write(block)
                        digest.update(block)
                        size += len(block)
                        if time.monotonic() - heartbeat_at >= 1:
                            self.executor.heartbeat()
                            heartbeat_at = time.monotonic()
                if self._run_identity(source) != current or size != current[2]:
                    raise AdapterError("run changed while collecting evidence")
                info = dict(
                    captured=True,
                    complete=True,
                    truncated=False,
                    source_bytes=size,
                    captured_bytes=size,
                    sha256=digest.hexdigest(),
                    reason="fresh_execution_run",
                    upload_supported=size <= 1024**3,
                )
        (directory / "evidence.json").write_text(json.dumps(info, indent=2), encoding="utf-8")
        return info

    def execute(self, task, directory):
        if not self.healthy:
            raise AdapterError("worker quarantined after cleanup failure")
        directory = Path(directory).resolve()
        phase_started = time.monotonic()
        command_completion = None
        finalize_started = None
        timing = None
        source = self.work_root
        protected = self.galaxcore_root or source
        if directory == protected or protected in directory.parents:
            raise AdapterError("output must not be inside slot inputs")
        directory.mkdir()
        result = dict(
            task_id=task["task_id"],
            worker_id=self.worker_id,
            revision=task.get("revision"),
            payload=task.get("payload"),
            status="FAILED",
            reason="worker_exception",
            raw_exit_code=None,
            exit_code=None,
            pid=None,
            started_at=time.time(),
        )
        # Kernel lock on the canonical source directory: identity is independent
        # of worker names or configurable lock-root aliases. Source is read-only.
        lock_fd = os.open(source, os.O_RDONLY | os.O_DIRECTORY)
        env = None
        prepared = False
        flow_original = None
        flow_restore_needed = False
        config_path = None
        launched = False
        selected = None
        status_directories = ()
        run_before = None
        with (directory / "stdout.log").open("xb") as output, (directory / "stderr.log").open(
            "xb"
        ) as err:
            try:
                fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                _, settings = self.validate(task)
                artifact = self.artifact(task["revision"])
                root, binary, manifest = self._prepare(artifact, task["revision"], directory)
                self.work_root = root
                case, _ = self.validate(task)
                namespace = "gcrp-" + uuid.uuid4().hex
                env = os.environ.copy()
                env.update(self.environment)
                # Do not allow inherited PJTest identities to select another run.
                for key in list(env):
                    if key.startswith(("PJTEST_", "DTS_")) or key in (
                        "GALAXCORE_WORKSPACE_ROOT",
                        "GALAXCORE_BIN",
                        "GALAXCORE_FLOW_CONFIG",
                        "GALAXCORE_WORKER_NAME",
                        "GALAXCORE_TASK_ID",
                        "GALAXCORE_EXAMPLE_ID",
                        "GALAXCORE_ATTEMPT_ID",
                        "GALAXCORE_BUILD_INFO",
                        "RUN_SH_LOCK_FILE",
                        "RUN_SH_LOCK_HELD",
                        "RUN_SH_LOCK_DIR",
                        "VIVADO_RUNNER_NAMESPACE",
                    ):
                        env.pop(key)
                env.update(
                    {
                        "PJTEST_TASK_ID": str(task["task_id"]),
                        "PJTEST_REVISION": task["revision"],
                        "PJTEST_WORKER_NAME": self.worker_id,
                        "GALAXCORE_RUN_MODE": "distributed",
                        "DTS_RUN_MODE": "distributed",
                        "GALAXCORE_BUILD_REVISION": task["revision"],
                        "GALAXCORE_REVISION": task["revision"],
                        "GALAXCORE_BUILD_ZIP": str(directory / "artifact.zip"),
                        "GALAXCORE_BIN": str(binary),
                        "GALAXCORE_WORKSPACE_ROOT": str(root),
                        "VIVADO_RUNNER_NAMESPACE": namespace,
                        "RUN_SH_LOCK_DIR": str(directory / "locks"),
                    }
                )
                config_path = root / "flow_config"
                if self.galaxcore_root is not None:
                    if config_path.is_file():
                        flow_original = config_path.read_bytes()
                        (directory / "flow_config.before").write_bytes(flow_original)
                    elif not settings:
                        raise AdapterError(
                            "slot flow_config missing; configure worker.flow settings"
                        )
                    flow_restore_needed = True
                if task["payload"]["spec_version"] == 2:
                    config_path.write_text(
                        "".join("{} {}\n".format(k, settings[k]) for k in sorted(settings)),
                        encoding="utf-8",
                    )
                elif settings or self.galaxcore_root is None:
                    self._write_flow_config(config_path, settings)
                shutil.copyfile(config_path, directory / "flow_config")
                result.update(
                    artifact=str(artifact),
                    manifest=manifest,
                    case=str(case),
                    workspace=str(root),
                    namespace=namespace,
                )
                prepared = True
                if self._run_clean(env, output) != 0:
                    raise AdapterError("pre_clean_failed")
                status_directories = self._status_directories(root, namespace, case)
                if any(
                    (path / "result.env").exists() or (path / "result.env").is_symlink()
                    for path in status_directories
                ):
                    raise AdapterError("result existed before execution")
                run_before = self._run_identity(case.parent / "run")
                try:
                    fingerprint = script_fingerprint(root, case)
                except (OSError, ValueError) as error:
                    fingerprint = ""
                    result["timing_warning"] = "fingerprint_unavailable:" + type(error).__name__
                effective_timeout, timeout_reason = select_timeout(
                    task, fingerprint, manifest["archive_sha256"]
                )
                timing = dict(
                    version=1,
                    scope=SCOPE,
                    prepare_wall_ms=max(0, int((time.monotonic() - phase_started) * 1000)),
                    effective_timeout_seconds=effective_timeout,
                    script_fingerprint=fingerprint,
                    archive_sha256=manifest["archive_sha256"],
                    fallback_reason=timeout_reason,
                )
                argv = [
                    "bash",
                    str(root / "run.sh"),
                    str(case),
                    "--timeout",
                    str(effective_timeout),
                    "--bg",
                    "1",
                    "--galaxcore",
                    str(binary),
                    "--flow-config",
                    str(config_path),
                ]
                result["argv"] = argv

                def record_start(pid):
                    nonlocal launched
                    launched = True
                    result["pid"] = pid
                    (directory / "pid").write_text(str(pid) + "\n")
                    (directory / "execution.json").write_text(json.dumps(result, indent=2))

                pid, code, timed_out = self.executor.run(
                    argv,
                    root,
                    env,
                    output,
                    err,
                    effective_timeout + self.grace + 5,
                    self.grace,
                    record_start,
                )
                command_completion = dict(self.executor.last_completion)
                finalize_started = time.monotonic()
                result.update(pid=pid, raw_exit_code=code, exit_code=code)
                selected = self._collect_case_result(
                    result, status_directories, case, directory, code, timed_out
                )
            except BlockingIOError:
                result["reason"] = "slot_busy"
            except AdapterError as error:
                result.update(status="FAILED", reason=str(error)[:160])
            except ExecutionContainmentError as error:
                self.healthy = False
                result.update(status="FAILED", reason=str(error), worker_healthy=False)
            except (OSError, ValueError, zipfile.BadZipFile) as error:
                result.update(status="FAILED", reason="worker_exception:" + type(error).__name__)
            finally:
                self.executor.stop(self.grace)
                if timing is not None and command_completion is None:
                    command_completion = dict(self.executor.last_completion)
                if finalize_started is None:
                    finalize_started = time.monotonic()
                try:
                    if prepared and self.executor.safe:
                        # Retain bounded diagnostic evidence before destructive cleanup.
                        case_run = self.work_root / task["payload"]["case"]
                        capture_failed = False
                        try:
                            result["evidence_capture"] = self._capture_run(
                                case_run, directory, run_before, launched
                            )
                        except (OSError, AdapterError) as error:
                            capture_failed = True
                            result["evidence_capture"] = dict(
                                captured=False, complete=False, reason=str(error)[:256]
                            )
                        # The randomized namespace isolates this run's runner
                        # logs from other cases sharing the SVN checkout.
                        namespace = result.get("namespace")
                        if namespace:
                            runtime = (
                                self.work_root
                                / "vivado_runner"
                                / "runtime"
                                / "workspaces"
                                / namespace
                            )
                            sources = [("runner.log", runtime / "logs" / "run.log")]
                            case_logs = (
                                [selected.parent / "run.log"]
                                if selected
                                else [path / "run.log" for path in status_directories]
                            )
                            for case_log in case_logs:
                                if case_log.is_file():
                                    sources.append(("case-run.log", case_log))
                                    break
                            for local_name, log_source in sources:
                                if (
                                    runtime.is_symlink()
                                    or log_source.is_symlink()
                                    or any(
                                        parent.is_symlink()
                                        for parent in log_source.parents
                                        if parent == runtime or runtime in parent.parents
                                    )
                                ):
                                    continue
                                try:
                                    if (
                                        log_source.is_file()
                                        and log_source.stat().st_size <= 1024**3
                                    ):
                                        shutil.copyfile(
                                            str(log_source), str(directory / local_name)
                                        )
                                except OSError as error:
                                    print(
                                        "[WORKER] runner log capture failed: " + str(error),
                                        flush=True,
                                    )
                        try:
                            # Don't destroy the original run if evidence capture failed.
                            clean_code = -1 if capture_failed else self._run_clean(env, output)
                        except Exception as error:
                            clean_code = -1
                            result["cleanup_error"] = str(error)[:160]
                        result["cleanup_exit_code"] = clean_code
                        if clean_code:
                            self.healthy = False
                            result.update(
                                status="FAILED", reason="post_clean_failed", worker_healthy=False
                            )
                finally:
                    if flow_restore_needed:
                        if self.executor.safe:
                            try:
                                if flow_original is None:
                                    config_path.unlink()
                                else:
                                    config_path.write_bytes(flow_original)
                            except OSError:
                                self.healthy = False
                                result.update(
                                    status="FAILED",
                                    reason="flow_restore_failed",
                                    worker_healthy=False,
                                )
                        else:
                            self.healthy = False
                    self.work_root = source
                    os.close(lock_fd)
                    if timing is not None and command_completion:
                        for key in ("command_wall_ms", "reap_wall_ms"):
                            if type(command_completion.get(key)) is int:
                                timing[key] = command_completion[key]
                        timing["finalize_wall_ms"] = max(
                            0, int((time.monotonic() - finalize_started) * 1000)
                        )
                        timing["supervisor_reason"] = command_completion.get("reason", "unknown")
                        if selected is not None:
                            try:
                                fields = dict(
                                    line.split("=", 1)
                                    for line in (directory / "result.env").read_text().splitlines()
                                    if "=" in line
                                )
                                runtime = int(fields.get("RUNTIME_SEC", "-1"))
                                if 0 <= runtime <= 7 * 86400:
                                    timing["runner_runtime_ms"] = runtime * 1000
                            except (OSError, ValueError):
                                pass
                        result["timing"] = timing
                        if isinstance(task.get("timeout_decision"), dict):
                            result["timeout_decision"] = task["timeout_decision"]
                    result["finished_at"] = time.time()
                    (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        return result
