"""Durable, allowlisted slot maintenance. Never runs server-supplied commands."""
import json
import os
from pathlib import Path
import re
import time
import uuid

from process_executor import ProcessExecutor
from recovery_journal import write_json

IDENTITY = re.compile(r"[A-Za-z0-9_-]{1,128}\Z")


class SlotMaintenance:
    def __init__(self, output, heartbeat):
        self.output = Path(output).resolve()
        self.heartbeat = heartbeat
        self.path = self.output / "maintenance.json"
        self.command = None
        self.state = None
        if self.path.is_symlink():
            raise RuntimeError("unsafe maintenance journal")
        if self.path.exists():
            self.state = json.loads(self.path.read_text(encoding="utf-8"))
            self.validate(self.state)
            if self.state["state"] == "RUNNING":
                receipt = self.state.get("receipt")
                proof = Path(receipt) if isinstance(receipt, str) else None
                if (
                    proof is None
                    or proof.parent != self.output
                    or proof.is_symlink()
                    or not proof.is_file()
                    or json.loads(proof.read_text(encoding="utf-8"))
                    != dict(clean=True, token=proof.name)
                ):
                    raise RuntimeError(
                        "maintenance exit unproven; quarantine slot; inspect " + str(self.path)
                    )
                self.state.update(
                    state="FAILED",
                    error="update interrupted; inspect SVN working copy before retry or resume",
                )
                write_json(self.path, self.state)

    @staticmethod
    def validate(value):
        if (
            not isinstance(value, dict)
            or not isinstance(value.get("operation_id"), str)
            or not IDENTITY.fullmatch(value["operation_id"])
            or value.get("scope") not in ("test2", "all")
            or value.get("state") not in ("RUNNING", "SUCCEEDED", "FAILED")
            or not isinstance(value.get("error"), str)
            or len(value["error"].encode("utf-8")) > 512
        ):
            raise RuntimeError("invalid maintenance journal")

    def acknowledgement(self):
        if self.state is None:
            return None
        return {key: self.state[key] for key in ("operation_id", "state", "error")}

    def offer(self, command):
        if command is None:
            return
        if (
            not isinstance(command, dict)
            or set(command) != {"operation_id", "scope"}
            or not isinstance(command["operation_id"], str)
            or not IDENTITY.fullmatch(command["operation_id"])
            or command["scope"] not in ("test2", "all")
        ):
            raise ValueError("invalid maintenance command")
        if self.state and self.state["operation_id"] == command["operation_id"]:
            if self.state["scope"] != command["scope"]:
                raise ValueError("maintenance operation_id reused")
            return  # Retried delivery: resend the durable outcome, never execute twice.
        if self.command is not None and self.command != command:
            raise ValueError("conflicting maintenance command")
        self.command = dict(command)

    def run_if_idle(self, adapter):
        if self.command is None:
            return False
        command, self.command = self.command, None
        self.state = dict(command, state="RUNNING", error="", receipt=None)
        write_json(self.path, self.state)
        executor = ProcessExecutor(self.heartbeat)
        executor.events_stderr = True  # Preserve machine-readable SVN stdout.

        def receipt():
            path = self.output / ("maintenance-receipt-" + uuid.uuid4().hex + ".json")
            self.state["receipt"] = str(path)
            write_json(self.path, self.state)
            return path

        executor.before_launch = receipt
        log = self.output / ("maintenance-log-" + command["operation_id"] + ".log")
        try:
            if adapter is None:
                raise RuntimeError(
                    "SVN update requires a complete PJtest Worker; demo execution is unsupported"
                )
            with log.open("a", encoding="utf-8") as stream:
                print(
                    "[MAINTENANCE] scope={} operation={}".format(
                        command["scope"], command["operation_id"]
                    ),
                    file=stream,
                    flush=True,
                )
                adapter.update_checkout(command["scope"], executor, stream)
            self.state.update(state="SUCCEEDED", error="")
        except Exception as error:
            if not executor.safe:
                # Never report completion, resume dispatch, or reuse an unproven workspace.
                raise RuntimeError("maintenance supervisor lost; quarantine slot") from error
            message = (
                (type(error).__name__ + ": " + str(error))
                .encode("utf-8", "replace")[:512]
                .decode("utf-8", "ignore")
            )
            self.state.update(state="FAILED", error=message)
        write_json(self.path, self.state)
        print(
            "[WORKER] maintenance {}: {}".format(self.state["state"], command["operation_id"]),
            flush=True,
        )
        return True

    def prune_logs(self, days):
        cutoff = time.time() - days * 86400
        current = self.state["operation_id"] if self.state else None
        for path in self.output.glob("maintenance-log-*.log"):
            identity = path.name[len("maintenance-log-") : -len(".log")]
            if (
                IDENTITY.fullmatch(identity)
                and identity != current
                and not path.is_symlink()
                and path.is_file()
                and path.stat().st_mtime < cutoff
            ):
                path.unlink()
        for path in self.output.glob("maintenance-receipt-*.json"):
            if (self.state and str(path) == self.state.get("receipt")) or path.is_symlink():
                continue
            if (
                re.fullmatch(r"maintenance-receipt-[0-9a-f]{32}\.json", path.name)
                and path.is_file()
            ):
                if path.stat().st_mtime < cutoff:
                    proof = json.loads(path.read_text(encoding="utf-8"))
                    if proof == dict(clean=True, token=path.name):
                        path.unlink()
