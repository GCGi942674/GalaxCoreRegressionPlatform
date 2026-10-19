# PJTest 定时批次提交

`server/clock.py` 读取 `server/Tasks.yaml` 与 `server/templates/*.json`，
一次选定一个 `GalaxCore_N.zip` 版本，展开 runlist 或目录中的 `run.tcl`，
然后通过 GCRP TCP+JSON 提交任务。每个 case 是独立的 GCRP Task；
每个 Tasks.yaml 条目对应一个父任务/批次，在 GUI 中汇总显示。
默认每个批次全部提交后暂停 30 秒，不是每个 testcase 等待 30 秒。
五个批次共暂停约 150 秒，另加网络和写库时间；`--interval 0` 取消批次暂停。

使用前在服务端配置 `server.work_root`（完整 GalaxCore checkout 中的 test2）、
`server.artifact_root`（ZIP 所在共享目录）、`server.protect_versions_file`
（构建清理脚本读取的保护版本文件）和 `server.clock_state_file`。
`server.tasks_yaml` 指向任务表，`server.templates_dir` 指向 JSON 模板目录。
`template` 值对应模板 JSON 的文件名前缀。缺少模板时 dry-run 失败，不猜测参数。
共享 ZIP 的生产和最近 50 个版本清理由外部构建流程负责；Clock 不删除 ZIP。

先检查配置，再启用定时：

```sh
python3 -B server/clock.py --dry-run
python3 -B server/clock.py --check-only
python3 -B server/clock.py --interval 30
```

需要 Python 3.6+ 和 PyYAML。可由系统 cron 每天调用一次，但首次启用前请确认
dry-run 成功、Worker 在线、Server 无未完成任务以及磁盘容量足够。
同一天重复调用会跳过，并有文件锁防止多个 Clock 并发提交。提交计划、批次 ID、
submission_key 和游标会持久保存；提交中断或 ACK 丢失后重新运行，会用原计划
和幂等键续传。Server 任务与停止标记存入 SQLite。不要删除 Clock 状态或 DB
来强行重跑。真实提交前还会创建一致性备份，dry-run/check-only 不创建备份。
当前强制 `max_retry=0`，不支持旧 PJTest 的失败重试语义。
批次运行中可用 `gcrp_client cancel-batch BATCH_ID` 停止剩余 case；
运行中的 case 会正常完成，详见 `docs/batch-stop.md`。
