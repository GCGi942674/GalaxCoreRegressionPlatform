# P10 批次停止

Clock 每次运行生成一次 invocation ID（输出的 `BATCH:`），每个 Tasks.yaml
条目另有独立父任务 `batch_id`，记录在状态文件的 `group_ids` 和该条目下每个
case 的 `payload.context.batch_id` 中。从 GUI 或状态文件复制父任务 ID 到 CLI：

```sh
./build/bin/gcrp_client query-batch BATCH_ID
./build/bin/gcrp_client cancel-batch BATCH_ID
```

取消在调度器锁内完成：同批 PENDING/QUEUED case 变成 CANCELLED，
ASSIGNED/RUNNING case 保持原状并等待工作机正常结果。已完成的 case 保留结果。
停止标记会拒绝此后同批的新提交；Clock 跳过该 YAML 条目的剩余 case，记录到
`stopped_groups`，但继续其他 YAML 条目，最后保存 `phase=complete`。重复取消返回当前统计，
`cancelled_now=0`。`complete=true` 只表示已创建的任务均已到终态。

批次、任务、取消标记和提交幂等键均持久保存在 SQLite；重启会恢复。若提交中断，
保留 Clock 计划/状态和 Worker journal，使用原 submission_key 续传，不要删除状态重跑。
单任务 `cancel TASK_ID` 仍只接受 PENDING/QUEUED；Worker 不接收停止进程指令。

旧版 Clock 创建的 case 使用日期和版本组合的批次 ID；新的唯一 ID 从下一次
运行开始生成，并写入状态文件。测试专用 `protect_versions_file` 不应充当正式
清理流程的版本保护文件。
