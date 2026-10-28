#!/usr/bin/env python3
"""Build a read-only Vivado timing index. Python 3.6+, no extra services."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sys
import tempfile

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / "worker"))
from runtime_config import load_config

OPERATIONS = {
    "read_edif": "read_edif",
    "read_xdc": "read_xdc",
    "report_timing_summary": "report_timing_summary",
    "report_utilization": "report_utilization",
    "opt_design": "opt_design",
    "place_design": "place_design",
    "place_design_from_syn": "place_design",
    "phys_opt_design": "phys_opt_design",
    "route_design": "route_design",
    "route_design_from_place": "route_design",
    "write_checkpoint": "write_checkpoint",
    "write_bitstream": "write_bitstream",
}


def workload_key(case, flow):
    data = json.dumps(
        dict(case=case, flow_config=flow, scope="single_case_runner_wall_v1"),
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")
    return hashlib.sha256(data).hexdigest()


def parse_elapsed(text, operation):
    # Only a completed, command-scoped wall elapsed is accepted. CPU is ignored.
    pattern = re.compile(
        r"^\s*" + re.escape(operation) + r"\s*:\s*Time\s*\(s\)\s*:\s*cpu\s*=.*?;\s*elapsed\s*=\s*"
        r"([0-9]+):([0-5][0-9]):([0-5][0-9](?:\.[0-9]+)?)(?=\s|;|$)",
        re.I,
    )
    found = []
    for number, line in enumerate(text.splitlines(), 1):
        match = pattern.match(line)
        if match:
            seconds = int(match.group(1)) * 3600 + int(match.group(2)) * 60 + float(match.group(3))
            if not math.isfinite(seconds) or not 0 < seconds <= 604800:
                raise ValueError("invalid elapsed")
            found.append((int(math.ceil(seconds * 1000)), number))
    if len(found) != 1:
        raise ValueError("missing or ambiguous command elapsed: " + operation)
    return found[0]


def build_index(config):
    baseline = config.get("server.vivado_baseline_root") or ""
    if not baseline:
        raise ValueError("configure server.vivado_baseline_root first")
    root = Path(baseline).expanduser().resolve(strict=True)
    work = Path(config["server.work_root"]).expanduser().resolve(strict=True)
    templates = Path(config["server.templates_dir"]).expanduser().resolve(strict=True)
    entries, failures = [], []
    skipped = 0
    cases = sorted(work.rglob("run.tcl"))
    if len(cases) > 50000:
        raise ValueError("catalog exceeds 50000 cases")
    for file in sorted(templates.glob("*.json")):
        template = json.loads(file.read_text(encoding="utf-8"))
        flow = template.get("flow_config")
        if not isinstance(flow, dict) or not flow:
            raise ValueError("invalid template flow: " + file.name)
        operations = [command for switch, command in OPERATIONS.items() if flow.get(switch)]
        if len(operations) != 1:
            skipped += len(cases)
            if len(failures) < 20:
                failures.append(
                    dict(
                        template=file.stem,
                        reason="scope_mismatch: requires exactly one primary operation",
                    )
                )
            continue
        name = template.get("vivado_baseline_log", file.stem + ".log")
        if not isinstance(name, str) or Path(name).name != name or name in (".", ".."):
            raise ValueError("baseline log must be a filename")
        for case in cases:
            relative = case.relative_to(work)
            source = root / relative.parent / name
            try:
                resolved = source.resolve(strict=True)
                if root not in resolved.parents:
                    raise ValueError("baseline escapes root")
                if resolved.stat().st_size > 16 * 1024 * 1024:
                    raise ValueError("baseline log exceeds 16 MiB")
                data = resolved.read_bytes()
                elapsed, line = parse_elapsed(data.decode("utf-8", "replace"), operations[0])
                entries.append(
                    dict(
                        workload_key=workload_key(relative.as_posix(), flow),
                        case=relative.as_posix(),
                        template=file.stem,
                        elapsed_ms=elapsed,
                        scope="vivado_stage_elapsed_v1",
                        sha256=hashlib.sha256(data).hexdigest(),
                        source=resolved.relative_to(root).as_posix(),
                        source_line=line,
                        parser_version="vivado-command-time-v1",
                    )
                )
            except (OSError, ValueError) as error:
                skipped += 1
                if len(failures) < 20:
                    failures.append(
                        dict(case=relative.as_posix(), template=file.stem, reason=str(error))
                    )
    unique = {}
    for entry in entries:
        old = unique.get(entry["workload_key"])
        if old and (old["sha256"] != entry["sha256"] or old["elapsed_ms"] != entry["elapsed_ms"]):
            raise ValueError("conflicting templates map to the same workload")
        unique[entry["workload_key"]] = entry
    return dict(version=1, entries=list(unique.values())), dict(
        imported=len(unique), skipped=skipped, examples=failures
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", help="Server config path")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if args.config:
        os.environ["GCRP_CONFIG"] = str(Path(args.config).expanduser().resolve())
    os.chdir(str(PROJECT))
    try:
        config = load_config("server")
        index, report = build_index(config)
        print(json.dumps(report, ensure_ascii=False, indent=2))
        if not index["entries"]:
            raise ValueError("no unambiguous timings; existing index was not replaced")
        if not args.dry_run:
            target = Path(
                config.get("timeout.baseline_index_file") or "./database/vivado-runtime-index.json"
            ).expanduser()
            target.parent.mkdir(parents=True, exist_ok=True)
            descriptor, name = tempfile.mkstemp(prefix=".vivado-runtime-", dir=str(target.parent))
            try:
                with os.fdopen(descriptor, "w") as stream:
                    json.dump(index, stream, ensure_ascii=False, indent=2)
                    stream.flush()
                    os.fsync(stream.fileno())
                os.replace(name, str(target))
            finally:
                if os.path.exists(name):
                    os.unlink(name)
            print("INDEX: " + str(target.resolve()))
        return 0
    except (OSError, ValueError, KeyError) as error:
        print("[VIVADO-TIME] " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
