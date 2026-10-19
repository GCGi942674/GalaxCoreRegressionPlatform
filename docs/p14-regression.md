# P14 Regression query v1

GCRP stores one `run.tcl` as one Task; PJTest stores a parent task,
examples, and attempts separately. This read-only view is deliberately based on
GCRP Tasks. It does not claim to reproduce PJTest's parent-task IDs,
example IDs, historical files or GUI cache.

`QUERY_REGRESSION` accepts JSON with at least one of `revision`, `case`, or
`batch_id`. `flow` may narrow that result. Optional `limit` is 1..50 (default
20); `before_task_id` is an exclusive descending cursor. Unknown fields,
empty filters and unbounded queries are rejected. The response contains
`total`, counts by outcome, `items`, `has_more`, and `next_before_task_id`.
Counts include all matching tasks, not just the current page. A missing match
returns an empty result, not a missing-task error.

The CLI exposes the same query:

```bash
./build/bin/gcrp_client regression --revision 18925 --limit 20
./build/bin/gcrp_client regression --case kintexuplus/xcku3p-ffvd900-1-i/convergentRoundingEven/run.tcl --flow route
./build/bin/gcrp_client regression --batch-id 2026-10-10-18925 --before 10001
```

Only Tasks containing a real `payload.case` participate; legacy demo Tasks
without a testcase are excluded. A completed case with `case_status=PASS`
is `PASS`; explicit `FAIL` and `CRASH` from the Worker remain distinct.
`TIMEOUT` and `CANCELLED` are preserved. Failed execution without an explicit
case outcome is `ERROR`, not a fabricated business failure or crash. Active
Tasks are `ACTIVE`. A success lacking a case verdict is `UNKNOWN`, not `PASS`.
The original `status`, `reason`, `case_status` and `case_reason` are retained in
each history item where present.

Scheduled-date queries now filter through the SQLite query index before loading
Task JSON; parent lists aggregate counters in SQL. Other broad revision/progress
queries can still scan history, and the global mutation lock is not eliminated.
Further optimization needs measured load. In particular, this is not yet a
replacement for PJTest's log download, detailed attempt history, or matrix
reports. Those require explicit data contracts and should not be inferred from
one Task's latest execution summary. Neither `result.env` files nor entire
worker logs are stored in the GCRP database.
