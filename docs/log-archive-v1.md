# Run log archive v1

Server stores evidence under `server.log_archive_root/TASK_ID/RUN_ID/`. Keep this
directory on durable storage. The default is `./data/run-logs`; it is separate
from the Server diagnostic `log.dir` and the Worker `worker.output_dir`.

After a terminal TASK_RESULT, the Python Worker writes an fsynced local outbox
entry, uploads files in 16 KiB hex-encoded chunks, and removes the outbox only
after Server acknowledgement. Disconnects and Server restarts resume at the
Server's durable offset. An idle Worker retries incomplete uploads. Upload
failure is visible in Worker output and does not change task PASS/FAIL/TIMEOUT.
The archive accepts both the current and persisted older finished run_id values
for a task, but only from that run's Worker connection.

The supported evidence set is `stdout.log`, `stderr.log`, `result.env`
(when present), `result.json`, `flow_config` (when present), and the full
`evidence/run` copy (`evidence.run` in the archive). It also captures the
namespace-isolated vivado_runner `logs/run.log` (`runner.log`) and the single
case `status/.../run.log` (`case-run.log`) before slot cleanup. This captures
run.sh output and core runner/case logs on success and failure. Other tool-
specific stage files and full-text search are not yet included. Each file is
limited to 1 GiB. Oversized files block cleanup of that task's local logs and
remain protected by the upload outbox; manual transfer is required.

## Verified upload and local cleanup

After upload, Worker sends each file's size and SHA-256. Server reads back its
stored bytes, verifies the hash, syncs the file and completion marker, and returns
`verified_sha256`. Slow verification keeps Worker heartbeats active. All files
must be confirmed before cleanup starts.

Worker durably saves the receipts in its pending outbox and writes `archived.json`
in the task output. Only then does it unlink known task log/flow/result files.
Interrupted cleanup resumes idempotently; a lost verification ACK, changed source,
old Server, upload failure or oversized file preserves the local logs. Unknown
files, ZIPs, SVN copies, service logs and recovery journals are not deleted by
this operation. Server retains its archives for 30 days; a successful verification
refreshes that retention window before Worker cleanup is authorized.

Raw run evidence is copied in full, not just its first 8 MiB. `evidence.json`
records size, SHA-256 and completeness. Only a run created/changed by this launched
execution is eligible; pre-clean failure cannot archive the previous task's run
as current evidence. SHA-256 provides transfer integrity, not authentication.

To inspect a completed task, get its `run_id` from `query TASK_ID`, then:

```sh
./build/bin/gcrp_client logs TASK_ID --run-id RUN_ID
./build/bin/gcrp_client log TASK_ID --run-id RUN_ID --name stdout.log --offset 0 --limit 8192
```

`log` returns `data_hex` in a JSON response; decode hex for display. Use
`next_offset` until it reaches `size`. An item is complete only when its
`complete` flag is true. Client reads are restricted to a finished run owned
by the requested task. Worker writes require its bound connection identity.
These role checks are **not authentication**: use the existing trusted-LAN
deployment boundary; do not expose this port to untrusted clients.

No log-ownership migration is needed: run ownership is looked up from existing
`task_runs`; archive bytes live in the configured directory. Back up that
directory together with SQLite if log retention matters. Deleting a task is
not implemented in this version. The accompanying task-query optimization does
migrate the query index and daily plan registry to schema v5; see production-hardening.md.

## Observable archival lifecycle and downloads

Worker registration replies advertise `archive_lifecycle_v1`. A new Worker uses
the bound Worker-only `log_state` operation to announce its exact allowlisted
file set before upload, then acknowledges local cleanup after it actually occurs.
The cleanup ACK is retried from the durable outbox if disconnected; it does not
repeat task execution or discard other files. Old Workers still upload normally,
but missing file-set or cleanup confirmation must not be inferred as success.

`query_logs.archive_state` is NOT_REPORTED, UPLOADING, UPLOADED_UNVERIFIED,
SERVER_VERIFIED, LOCAL_CLEANED or EXPIRED. NOT_REPORTED is deliberately ambiguous:
there is no proof of upload or expiration. EXPIRED requires the retention cleaner's
small durable tombstone. Retention removes log bytes, not that metadata. Per-file
`sha256` and `verified` come from fsynced Server receipts, not Worker assertions.
All files announced in the set must be verified before SERVER_VERIFIED; this is
transfer completeness, not proof that the runner generated every imaginable file.
Check evidence.json for raw run capture completeness.

Client read pages now allow 1..131072 bytes (uploads remain 16 KiB).
Qt downloads are disk-streamed up to the archive's 1 GiB per-file limit. On network
failure, choose the SAME destination to resume its .gcrp-part file. Matching
Task/Run/name/digest metadata and a local lock are required; the prefix is rehashed.
The complete SHA-256 must match before the original destination is atomically
replaced. A different identity, corrupted content or disk-full error preserves
the destination and temporary evidence; inspect stale partial files explicitly.
Final publication temporarily needs space for both the partial and final file.
Legacy files without a Server SHA-256 receipt can still be downloaded after an
explicit warning, using a fresh full transfer with no resume. The GUI never
labels those downloads verified or authorizes any Worker deletion from them.
Files above 1 GiB still require manual transfer and block Worker task-log cleanup.

## 30-day log retention

`log.retention_days=30` is shared by Server and Worker (default 30 when absent).
The persistent Server checks at startup (within one second) and hourly:
recognized files under `server.log_archive_root/TASK_ID/RUN_ID`, using the
newest file's modification time, and `server.log`, `task.log`, `error.log`
under dated `log.dir/YYYYMMDD` folders. Unknown files, symlinks and unexpected
directory names are left untouched. The Worker checks while idle, then hourly,
and removes only old per-task `worker.output_dir/run-*/TASK_ID` execution
directories. Active journals, pending results and pending log-upload outboxes
protect their task output. This policy does **not** remove SQLite task/history
records, SVN slot checkouts, or test/acceptance reports under `intranet-report`.
It also does not erase tool-specific files inside the SVN checkout; those are
managed by the runner's existing cleanup. Backups follow their own retention
policy. File age is based on filesystem timestamps; late uploads are retained
from their upload time.
