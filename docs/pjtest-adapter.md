# PJtest Adapter — 执行契约

每个 slot 独立执行单个 case；多 slot 管理由 Worker Manager 负责。
平台已包含 SQLite 和恢复机制，详见对应阶段文档；本文仅说明 Adapter 契约。
取消只停止尚未执行的 case，不终止 ASSIGNED/RUNNING 的真实进程。

## 启动配置

生产模式直接读取共享配置；演示必须显式 `--demo`，且拒绝业务 payload。
Server/CLI 使用 `config/server.conf`，Worker 使用 `config/worker.conf`；
旧单文件 `config/gcrp.conf` 仍兼容。多机部署时可用
`GCRP_CONFIG` 指向每台机器自己的配置副本：
`server.bind_ip` 是监听地址，`network.server_ip` 是 Client/Worker 连接地址，
`server.port` 为同一个端口。默认只监听本机；不要在无认证/TLS 的情况下开放公网。
Worker 的 `worker.output_dir`、`worker.slots_root` 和
`worker.artifact_root` 路径在该配置中集中指定；
相对路径均相对于进程启动目录。

工作机配置例子（从 GCRP 仓库根目录启动）：

```ini
network.server_ip=192.168.10.20
server.port=18080
worker.output_dir=./worker-output
worker.slots_root=./worker/workers_slots
worker.svn_url=http://192.168.10.10/svn/galaxcore/galaxcore
worker.svn_revision=
worker.artifact_root=/共享目录/构建包
worker.clean=true
worker.terminate_grace_seconds=30
# flow_config 由 Server 模板下发，工作机无需设置 worker.flow.*
```

`flow_config` 由服务端 `server.templates_dir` 中的同名 JSON 模板确定。
普通 CLI 提交 `--flow route` 时，Server 读取 `route.json`，把内容快照
写进任务 payload；Worker 严格按该快照覆盖 slot 的 test2/flow_config，
运行结束后恢复原文件。修改模板只影响新任务。Server 与 Worker 需一起更新；
旧版 Server 的 v1 payload 会被新版生产 Worker 拒绝。Worker ID 为
`pjtest-slot-1` 时，首次启动会 checkout 整个 SVN 仓库到
`worker/workers_slots/pjtest-slot-1/galaxcore`；再次启动复用该副本。
需要更新副本时由操作员在停机后检查并执行 SVN 更新，Worker 不会自动
覆盖已有副本。SVN 若需要认证，须预先配置工作机账号的 SVN 凭据。

```sh
python3 worker/gcrp_worker.py --worker-id pjtest-slot-1
./build/bin/gcrp_client submit --target xcvu9p --revision 58231 --case cases/smoke/run.tcl --flow route --case-timeout 600
```

无需单独编写 PJtest JSON 配置。`--pjtest-config` 仅保留给旧版离线夹具。
环境变量由启动 Worker 的 shell 继承；不会自动 source .cshrc。
`--output` 可临时覆盖 Worker 输出目录。对规范化输入目录本身加内核 flock：
对规范化输入目录本身加内核 flock，锁与 Worker 名称无关。
target 当前是业务标签，不隐式改变 case/器件/flow。

每个 slot 是完整 GalaxCore SVN 工作副本（其下有 `.svn` 和
`test2/run.sh`）。原 PJTest 允许 checkout 后暂时没有
`test2/flow_config`；Worker 会按 Server 下发的配置生成该文件，并在安全清理后
恢复原状。已有但不完整的目录
不会被自动覆盖或删除。每个 Worker 进程仍只有一个 slot，多 slot 应使用各自
独立的副本、worker ID 与输出目录。输出目录不能放进该副本。

此模式下 ZIP 只需一个 `GalaxCore` 可执行文件，`flow` 仍来自 slot 的
SVN 工作副本，不会被 ZIP 覆盖；相邻 manifest 可选。没有发布方 manifest
时记录 `worker_observed`、ZIP 哈希和 run.sh 哈希，但这**不能证明**
ZIP 与所填 revision 的真实对应关系，须由内网发布流程核对。安装前拒绝
目录穿越/链接和仍在使用旧二进制的进程；执行期间使用原 slot 的 test2，
原有 flow_config 在安全清理后恢复。完整 slot 模式不提供文件系统级隔离，
只适用于可信 SVN 工作副本/脚本。

结果先原子落盘为输出目录下按 worker ID 命名的 pending-report 文件，再发
`TASK_RESULT`。回执丢失时保留文件；同 ID Worker 重启并注册后先重放，
服务端对相同 worker、状态、结果摘要作幂等确认。若服务端重启丢失了内存
任务，重放会被拒绝并保留文件，需要人工核查，**没有自动恢复**。

以下“必须有 manifest、私有 workspace、ZIP flow”规则仅适用于
`--pjtest-config` 指定 `test2_root` 的旧离线模式，不能用作真实工作机部署要求。

## 旧离线模式的构建发布契约

不再接受裸 ZIP。每个 revision 必须只有一个候选：
GalaxCore_REV.zip、Galaxcore_REV.zip、GalaxCore_rREV.zip、GalaxCore-REV.zip。
多个候选、链接、缺少清单或哈希不匹配都会失败，不回退旧安装。

ZIP 相邻文件 ZIP完整文件名.manifest.json 必须包含：
spec_version=1、revision、test2_revision、archive_sha256、test2_sha256。
两个哈希都是 64 位小写 SHA256。发布者完成并核验构建后，先将 ZIP 复制到临时名，
完成后原子改名，最后发布清单：

```sh
python3 worker/publish_artifact_manifest.py --archive /local/artifacts/GalaxCore_58231.zip --revision 58231 --test2-root /local/inputs/test2 --test2-revision 58231
```

工具原子发布且拒绝覆盖已有清单。版本标签必须来自可信构建流程，不能通过
重命名旧 ZIP 冒充新版本；工具不会读取 ELF 来猜测 SVN 版本，也不运行 automation。
不同 GalaxCore/test2 revision 可显式记录，但操作员必须审核兼容组合。
清单是完整性契约而非数字签名，发布目录只允许可信构建方写入。

test2 摘要覆盖路径、内容、大小、权限与目录结构，排除 .svn、__pycache__、
vivado_runner/runtime。符号链接和特殊输入文件拒绝。输入必须是固定干净快照，
输出不得放进输入树。

## 隔离与结果归属

每任务在输出目录创建私有 workspace/test2、workspace/bin、workspace/flow。
ZIP 先复制再验哈希；test2 复制后再次验摘要；完整解包到新目录，不叠加旧 flow。
不修改输入 flow_config，不运行输入目录的 clean.sh。pre/post-clean 只操作副本。
ZIP 上限为 2 GiB 压缩大小、2 GiB 解包大小、100000 项，拒绝越界、链接和重名目标。

每轮生成唯一随机 namespace，只读取该副本 namespace 中匹配 case 的 result.env。
不遍历其他 namespace，不靠 mtime 判断归属。所有后代退出后才读取结果。
后台延迟写入不被视为合法的任务完成。RUN_TCL 必须匹配副本 case；
结果最多 64 KiB，拒绝链接，安全解析普通 shell 转义，不 source/eval。
特殊 ANSI-C 引号尚未完整兼容，内网优先使用普通 ASCII 路径。

显式传入 --bg 1、--galaxcore、--flow-config 和 --timeout，并固定本轮 workspace/
namespace。旧的身份、workspace 和锁环境不能改变本轮选择。许可证/库等其他环境
仍需通过启动环境或本地 environment 配置提供。

副本不等于文件系统沙箱：可信脚本内部写死的绝对路径/外部服务副作用仍需检查。
执行目录保留用于诊断，会增加磁盘占用。当前不自动递归删除，操作员须在确认
进程退出后归档清理历史运行。

## 进程生命周期

每条命令由独立 Linux subreaper 监督进程拥有，覆盖 setsid、double fork 后代。
Worker SIGTERM、断连会关闭租约管道；Worker SIGKILL 时内核也关闭租约，
监督进程继续 TERM/KILL 并回收后代。正常父脚本退出而后代仍存活，回收后返回
execution_tree_incomplete，不能被 PASS 覆盖。

只有 waitpid 确认 ECHILD 才发送完成凭证。凭证缺失禁止后续清理/终态上报，
避免释放仍可能有进程的 slot。PID 文件记录监督进程 PID，stdout 的
execution_started 另记录 command_pid 与 argv。

不要主动 SIGKILL 监督进程。监督进程被杀、系统重启、/proc 不可访问等情况下，
不能保证自动回收；缺失凭证会触发安全停止。不可中断 I/O 子进程无法退出时，
宁可继续占用 slot，不能假装完成。这不是对抗恶意同 UID 进程的安全沙箱。

## 清理失败与诊断

保留 execution.json、result.json、原始有效 result.env、实际 flow_config、
manifest.json、stdout/stderr、ZIP 与私有 workspace。post-clean 前完整复制本次新生成
或修改的 testcase run，不再仅保存前 8 MiB；采集元信息写入 evidence.json。
执行未开始或 run 未变化时不会把旧 run 当作本次证据。证据复制失败时保留源文件
并隔离，不能继续清理原文件。成功归档且 Server 校验通过后，已上传的本地日志会清理，
详见 log-archive-v1.md；ZIP/workspace 不在该自动清理范围。

post-clean 非零/异常、监督凭证缺失会隔离 Worker：不发送释放 slot 的 TASK_RESULT，
Worker 断开。Server 可能继续保持 RUNNING 和 slot 占用，后续任务排队。
这是安全停止，不是故障恢复。必须先核查进程和工作目录，不能换 Worker ID/
盲目重投绕过。CLI 目前不返回完整业务失败详情，需要查看 Worker 本地证据。

## 验证

```sh
ctest --test-dir build --output-on-failure
ctest --test-dir build -R 'test_python_worker|test_pjtest_adapter|test_worker_safety|test_server_flow|test_clock' --output-on-failure
```

tests/test_worker_safety.cpp 调用 Python 夹具，覆盖隔离、锁、哈希/清单/ZIP、
旧 flow、外来 namespace、低精度时间戳、清理隔离、Demo 拒绝、逃逸进程、
强杀 Worker、启动退出竞态和监督凭证丢失。reference 存在时还执行真实
run.sh/runner + 假 GalaxCore；未安装 reference 时明确 skip 该可选用例。
网络测试确认清理失败后任务仍 RUNNING，后续任务 QUEUED。

这不等于真实 GalaxCore/Vivado 验收；内网还需验证动态库、许可证、真实子进程树、
脚本绝对路径、共享文件系统 flock 支持、实际版本与磁盘容量。
