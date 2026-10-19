# GCRP Server API v1 (P17)

## Transport and compatibility

The existing TCP protocol is unchanged: 4-byte big-endian body length, then UTF-8
JSON, maximum body 1,048,576 bytes. Request envelope:

```json
{"version":1,"type":"query_tasks","request_id":42,"data":{"limit":20}}
```

Response envelope has type `response`, the same request_id, and data containing
`code`, `message`, `result`, `error_reason`. Router responses additionally include
`server_instance_id`, an opaque identifier stable within a Server process. Protocol
errors rejected before routing may omit that identifier. Clients must tolerate
additive fields and must not branch on the human-readable message.

| code | error_reason | meaning |
|---|---|---|
| 0 | OK | successful request |
| 1 | INVALID_REQUEST | bad fields, type, role or unsupported operation |
| 2 | NOT_FOUND | requested object missing |
| 3 | INVALID_STATE | operation forbidden in current state |
| 4 | INTERNAL_ERROR | server could not safely perform the operation |

Error reasons are deliberately categories, not English-message aliases. A role
check is NOT authentication: deployment remains a trusted-network service.

## Query endpoints

All filters are exact matches; limits apply to returned rows, not `total`.
Cursor requests must repeat their original filters. Unknown request fields are
rejected. Lists return empty arrays and null next cursors when exhausted.

| Operation | Request | Result / order |
|---|---|---|
| query_capabilities | `{}` | api_version, protocol_version, operations, limits, features |
| query_status | `{}` | task counters, workers, machines, regression_cache; legacy overview, not paginated |
| query_task | task_id | task detail, including payload and available execution evidence |
| query_tasks | optional status,target,revision,batch_id,view=groups,limit,before_task_id,snapshot_generation | bounded immutable query snapshot; task_id descending |
| query_workers | optional machine_id,status,limit,after | items,total,has_more,next_after,snapshot_generation,inventory_scope; machine_id/slot_id/worker_id lexicographic |
| query_batch | batch_id | batch counts, stop_requested,in_flight,complete; no task list |
| query_regression | revision or case or batch_id required; optional flow,limit,before_task_id | filters,total,counts,items,has_more,next_before_task_id,snapshot_generation; task_id descending |
| query_progress | target required; optional flow,limit,before_revision | target,flow,total_revisions,revisions,has_more,next_before_revision,snapshot_generation; numeric revision descending |
| query_rollback | target,from_revision,to_revision required; optional flow,limit,after_task_id | total,items,has_more,next_after_task_id,snapshot_generation,from_available,to_available; current_task_id ascending |
| query_logs | task_id,run_id | archived file names, sizes and completion flags for one finished run |
| read_log | task_id,run_id,name,offset,limit (1..131072) | binary-safe hex page, next_offset, size, completion flag and verified sha256 when available |
| query_daily_plan | date, optional offset (>=0),limit | plan_available, per-YAML-batch completeness and archive counters; original YAML order |
| query_catalog | optional offset,limit,generation,refresh | Server testcase tree paths, recent/protected ZIP versions and flow templates |

Default page size is 20; maximum 50 (progress maximum 20). Numeric cursors are
positive uint64 integers, not strings or floats. Progress revisions are decimal
strings up to 32 bytes, normalized by removing leading zeroes; from < to.
Text filters cannot be supplied as empty strings. Task target/revision/batch_id
limits are 128 bytes; status 16. Regression case limit is 512 and flow 64.
Worker query machine/cursor fields are bounded to 256 bytes.

Worker `after` is the exact object returned in `next_after`:

```json
{"machine_id":"novarch4","slot_id":"slot1","worker_id":"novarch4-slot1"}
```

Task list summaries always contain task_id,task_type,status,target,revision,
batch_id,worker_id,run_id,priority,retry_count. Unassigned identifiers are empty
strings. Lists include bounded payload/context/execution summaries and timestamps;
summary_truncated flags omitted oversized fields. query_task provides the full
flow snapshot; list summaries must not be used to reconstruct an execution.
Query_task retains its existing optional-field
contract: run_id, execution and timestamps may be absent before available;
recovery_required/recovery_reason are supplied when reconciliation is necessary.
Never interpret absent execution as a successful case.

Worker rows contain worker_id,machine_id,slot_id,hostname,status,capacity,used,free,
last_heartbeat_ms. Status is ONLINE, OFFLINE or DRAINING. `free` is capacity minus
usage, NOT proof that dispatch is allowed: OFFLINE/DRAINING must not receive new
tasks. Offline records remain visible within the process. They are not a durable
machine inventory: after restart, workers re-register. The capability
worker_inventory_persistent is false and inventory_scope is current_server_process.

## Consistency and business interpretation

Each paginated endpoint reads one locked snapshot. Task-derived endpoints use the
Scheduler generation; Worker queries use a separate registry generation (including
heartbeats). Generations are only comparable within the same domain and
server_instance_id. query_tasks retains immutable query snapshots for two minutes,
up to eight within a shared 64 MiB budget. Send snapshot_generation on later pages;
expiration/eviction fails explicitly. Group view returns one representative with
group_summary per parent, not one visible parent per testcase.
Other queries can change counts/membership between pages. A GUI can restart
pagination after a generation change; it must not silently merge pages across
Server restarts. Query_status is
a monitoring overview, not a transactionally consistent join of tasks and workers.

Progress is **target scoped, not project scoped**. Existing manual submissions may
use a device target while clock submissions use a template name. This API does
not silently merge those populations. Progress compares latest task_id for each
case+flow at each observed numeric revision. Rollback means confirmed PASS -> FAIL;
TIMEOUT, CRASH, ERROR, missing cases and incomplete runs are not counted as FAIL.
`from_available` / `to_available` distinguish missing revision data from zero
regressions. Observed adjacent revisions are not proof of an exact SVN boundary.

Server timestamps and Worker evidence timestamps come from different hosts;
do not subtract them to infer durations when clocks are unsynchronized.

## Mutation semantics (unchanged)

- submit_task: task_type,target,revision, optional priority,payload,submission_key.
  Returns task_id. Reuse an identical submission_key for an explicitly supported
  idempotent submission; do not blindly retry arbitrary mutations.
- publish_daily_plan: date,suite_id,plan_sha256,batches. Each batch contains
  batch_id,template,name,revision,planned (1..100000; suite total <=100000,
  at most 1000 batches). Immutable per date; exact replay is idempotent.
  Published before any Clock testcase; schema v5 persists it across restart.
- query_daily_plan counts scheduled tasks only. Complete requires exact planned
  count with no active task; zero-record planned batches remain visible.
  plan_available=false means unknown/legacy/not-published, never success.
- cancel_task: task_id. Only PENDING/QUEUED can be cancelled. Running/assigned
  executions are not killed and their slots are not released prematurely.
- cancel_batch: batch_id. Stops queued/pending cases and future submissions into
  that batch; in-flight cases finish. Returns the batch snapshot.
- Worker registration/heartbeat/recovery/drain/start/result protocol is unchanged.
  Worker-role connections cannot use Client query APIs.
- log_append is Worker-only and requires the bound worker_id plus a finished
  task_id/run_id owned by that Worker. Each request carries at most 16 KiB of
  binary evidence encoded as hex. The server returns its durable next_offset
  for idempotent resume. Details and current evidence scope: log-archive-v1.md.
- log_state: Worker-only, task_id,run_id,worker_id,state. State contains phase
  (UPLOADING or LOCAL_CLEANED) and the exact allowlisted file-name array.
  Local cleanup confirmation is accepted only after all expected files have
  Server verification receipts. Registration advertises archive_lifecycle_v1.

The Qt frontend is documented separately. Remote process cancellation, machine
power control, authentication and durable Worker inventory remain unavailable.

## CLI and acceptance

```bash
./build/bin/gcrp_client capabilities
./build/bin/gcrp_client tasks --status QUEUED --limit 20
./build/bin/gcrp_client workers --machine-id novarch4
./build/bin/gcrp_client status
```

The CLI retains exit codes 0 success, 2 Server rejection, 1 transport failure,
64 local usage error. New list filters are validated by the Server; invalid JSON
cursor syntax is a local usage error. Addresses still come from shared config.

`test_api_queries` covers routing, role permissions, empty lists, strict filters,
pagination and retained offline workers. `test_client_cli` exercises these commands
over framed TCP. Run both Debug and Release (checks never depend on assert side
effects). Run existing P10-P16 tests as regression coverage. For CMake 3.15, enter
the build directory before running ctest; do not use --test-dir.
