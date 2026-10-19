# P11 SQLite task state

`database.path` in the shared config selects the Server-local SQLite file. Install
the SQLite development package (`libsqlite3-dev` on Ubuntu, `sqlite-devel` on
RHEL/CentOS) before building. CMake 3.15.2 is supported. The Worker never opens
this file. Keep the database, its `-wal` and `-shm` files on the Server's local
disk; do not put them on the shared artifact directory.

The Server opens one database connection, takes an exclusive instance lock,
enables WAL, `synchronous=FULL` and foreign keys, and checks `user_version`.
An unreadable database or unknown schema version prevents startup. Schema V1
contains `batches`, `tasks`, `task_runs`, and `submission_keys`.

Current schema is v5: it also retains recovery/Worker management state, immutable
`daily_plans`, and a transactional `task_query_index` for parent summaries and
scheduled-date queries. Upgrade from v1-v3 backfills the derived index; v4 gains
the daily plan table without changing task IDs/results.
Back up the stopped Server's database before upgrading; older executables refuse
the upgraded DB. Never remove a live DB/WAL or edit user_version to force rollback.

Protocol requests are processed in receive order on business worker threads;
each connection has a bounded message queue (4 MiB, 512 messages). Database
syncs do not run in the socket I/O loop.

Each accepted task, assignment, start, completion, cancellation and batch stop
is committed before a success response or task dispatch. A batch stop writes
its marker and cancels all still-queued tasks in one transaction. SQL failure
leaves the scheduler state unchanged. The transaction does not contain network
I/O. A terminal result is saved before releasing its slot or acknowledging the
Worker. Repeated results for the same `run_id` and same result are accepted.

Each assignment receives a new `run_id`. A failed dispatch is recorded as
`DISPATCH_FAILED` and requeues the task with a new FIFO sequence unless the
batch was stopped. Old run results cannot finish a newer assignment. The
Worker persists a pending report locally and removes it only after the Server
acknowledges it.

On restart, QUEUED tasks regain their priority/FIFO order and batch stop
markers are restored. Prior ASSIGNED/RUNNING tasks retain their last state and
show `recovery_required=true` in QUERY_TASK. They are never automatically
requeued. Workers associated with such tasks are quarantined from new dispatch
until the pending result is reconciled. Full worker-loss reconciliation and
automatic retry belong to P12.

Clock saves the exact planned requests alongside its state file. Each case has
a stable `submission_key`; replay after a lost response returns the original
task ID. A key reused with different request data is rejected. Clock resumes
an interrupted `submitting` phase from the saved plan and repeats the uncertain
case with the same key. Do not remove the plan file while that phase is active.

Before replacing an in-memory deployment, back up the existing config and
Clock state file, choose a local `database.path`, stop the old Server, build
with SQLite headers available, then start the new Server. Tasks created by the
old in-memory Server cannot be reconstructed from the new empty database.
