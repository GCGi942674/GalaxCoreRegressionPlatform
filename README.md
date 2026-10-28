# GalaxCore Regression Platform

用于 GalaxCore 回归测试的调度平台：C++ Server、Python Worker、Qt5 GUI。

## 1. 编译

在项目根目录执行。需要 C++17、CMake 3.15.2+、SQLite3 开发库；
GUI 需要 Qt5 5.8+ Widgets/Network，Python 需要 3.6.8+。

```bash
make                         # 默认 32 线程；安装 Qt5 时一起编译 GUI
make GUI=OFF                 # 不编译 GUI
make GUI=ON PARALLEL_JOBS=4   # 必须编译 GUI，使用 4 线程
```

Worker 不需要编译 C++，需具备 SVN、Bash 和实际 GalaxCore/PJtest 运行环境。

## 2. 配置

只需修改两份配置：

- Server：`config/server.conf`，设置监听 IP、端口、test2 工作目录、ZIP 目录和 flow 模板。
- Worker：`config/worker.config`，设置 Server IP/端口、SVN URL、ZIP 目录和 `worker.slots` 数量。

`worker.svn_revision` 可留空，不要填写目录。主机名和 slot 名称自动生成；
每个 slot 使用独立完整 SVN 副本，已有副本会复用。flow_config 由 Server 下发。

## 3. 启动和停止

以下命令均在项目根目录执行。

**Server：**

```bash
./server/start.sh --config config/server.conf
./server/status.sh --config config/server.conf
./server/stop.sh --config config/server.conf
```

**Worker：**

```bash
./worker/start.sh --config config/worker.config
./worker/status.sh --config config/worker.config
./worker/stop.sh --config config/worker.config
```

**GUI：** 在有桌面环境的机器运行，替换为实际 Server 地址。

```bash
./build/bin/gcrp_gui --ip 192.168.10.11 --port 8080
```

GUI 可查看工作机、勾选 testcase 提交任务、查询任务/每日回归和查看执行日志。
每日回归支持日期、模板阶段、testcase 搜索自动刷新；统计卡覆盖全部匹配结果，
结果表不重复显示阶段列。“两天日志”仅比较真实归档的 run 和 flow_config。
本轮每日回归界面升级需一起更新 Server 与 GUI，Worker 无需更新。
“下线”只停止分配新任务，当前案例正常结束；“上线”恢复已连接 slot 的调度。
失联任务可用“故障恢复…”处理，但必须先确认原机器已关机或相关进程已停止。
恢复后保留旧执行记录，故障 slot 保持下线；取消批次不重排，超过重试上限转失败。

## 4. 每日定时回归

配置 `server/Tasks.yaml` 和 `server/templates/<模板名>.json`，先预览：

```bash
env GCRP_CONFIG=./config/server.conf python3 -B server/clock.py --dry-run
```

Clock 还需要 PyYAML。确认无误后，在 Server 的 crontab 添加一条每天 10:00 的任务，
将下面的项目路径替换为实际绝对路径：

```cron
0 10 * * * cd /你的项目路径/GalaxCoreRegressionPlatform && /usr/bin/env GCRP_CONFIG=./config/server.conf /usr/bin/python3 -B server/clock.py >/dev/null 2>&1
```

每条 Tasks.yaml 配置对应一个批次，批次之间默认等待 30 秒。
每日提交日志位于 `logs/daily/`；不要重复添加旧定时项或删除 Clock 状态强制重提。

## ZIP 版本保护

`server.protect_versions_file` 必须指向外部 ZIP 清理程序实际读取的文件。
文件由 Server 统一维护，每行一个纯版本号：最近一次已提交的每日回归版本、
全部未完成任务的版本，以及尚在提交中的每日/计划批次所需版本都会保留。
人工任务、复测、排队、运行和失联恢复均适用；全部结束后约 30 分钟解除自动保护。
下一轮每日任务完成提交后才交接每日保护，Server 重启从 SQLite 恢复。
Clock 不再直接写保护文件；升级时一起替换 Server 和 clock.py，Worker 无需更新。

如需人工固定保护，在保护文件旁建立 `<保护文件>.manual`，每行一个版本号。
该文件存在时作为人工名单的完整来源，空文件表示清空人工保护；不要直接编辑自动名单。
首次升级保留旧保护内容：从 Clock 状态识别每日版本；没有 Clock 状态时，旧名单仅一行则
按每日保护导入，其余按人工保护导入。保留原数据库、Clock 状态和保护文件再升级。
`server.protect_grace_seconds` 默认 1800，允许 60～86400 秒。

外部清理程序须支持所有行，并使用同一个 `<保护文件>.lock` 的 Linux `flock` 锁。
检查候选版本与删除对应 ZIP 必须处于同一次持锁期间；不要删除或替换锁文件。
两个进程须使用同一账号或具备锁文件读写权限的共享用户组，文件系统须支持该锁。
可复用 `tools/protected_versions.py` 中的 Python 3.6+ 辅助函数：

```python
import os
from protected_versions import locked_protected_versions

# candidates 仍由清理程序按“保留最近 50 个”规则产生。
for revision, zip_path in candidates:
    with locked_protected_versions('/实际共享路径/protect_versions') as protected:
        if revision not in protected:
            os.unlink(zip_path)
```

读取失败、文件缺失、内容非法或加锁失败时，清理程序必须跳过删除并报错。
Server 遇到锁忙或保护写入失败会拒绝本次新提交并保留已有任务，之后可重试。
实际总保留数为“最近 50 个 + 保护名单中额外的版本”，可能超过 51 个。
此机制无法找回升级前已被删除的 ZIP；外部脚本完成锁协作前，仍存在清理竞态。

## 代码格式

自有 C++/Qt 代码使用 clang-format 16，Python 使用 Black 23.12.1，统一 4 空格缩进、
100 字符目标行宽，短函数与控制语句展开。配置见 `.clang-format`、`.editorconfig` 和
`pyproject.toml`。字符串和注释可能保留长行，以避免改变日志、界面文本或说明内容。
第三方库、构建目录和运行数据不参与格式化。

开发机安装格式工具后执行：

```bash
python3 tools/format_code.py          # 统一排版
python3 tools/format_code.py --check  # 仅检查
```

Black 运行使用开发机 Python 3.8+，生成的业务代码仍保持 Python 3.6.8 兼容。
正式部署和 Worker 启动不依赖这些格式工具。

## 注意

### 全开 / 半开与每周计划

更新 Server、GUI 和 Worker 后，使用 `worker/start.sh` 启动工作机。在“工作机状态”选择主机，
可点 Auto、半开、全开或“每周计划”。Auto 默认周一至周五 09:00–19:00 半开，其他时间及周末
全开，以 **Server 系统时区** 为准；计划可按主机编辑并存入 SQLite。
半开默认允许 `slot1` 到 `slot ceil(N/2)`，也可指定数量；其他 slot 保持通信但禁用上线。
半开 / 全开快捷按钮临时生效 4 小时，之后恢复每周计划；弹窗可改为 1–168 小时。
模式不会覆盖人工下线，也不会中断已分配或正在运行的案例。全部上线只恢复当前模式允许的已连接 slot。

模式上限不等于立即启动数量：默认首次取得安全资源样本从 1 个并发起步，稳定 3 分钟后逐个增加；
内存可用低于 20% 持续 60 秒收缩（调整间隔至少 120 秒），低于 10% 或执行盘空闲小于 5 GiB
立即停止新增派发。恢复需内存可用至少 30%、CPU 小于 85% 且无持续换页/阻塞；每次增加后继续观察。
CPU 忙只阻止扩容，不单凭 CPU 使用率缩容。PSI 可用时辅助判断内存/IO 阻塞，不可用时仍可运行。
这些是试运行默认值，未针对内网案例峰值内存调优，不能保证避免 OOM；不会主动杀进程释放资源。
`config/server.conf` 的 `policy.*` 可调整基础阈值。GUI 可关闭自适应收缩/恢复，但严重资源不足仍保护；
关闭整个运行策略则退回纯人工控制。

每台主机只有一个后台采样线程，约每 10 秒采样；各 slot 共享上报。样本过期/无效暂停新增派发，
不会断开或杀掉运行任务。Server 重启后未重连的活动执行仍保留并发预算，直到完成恢复或故障确认。
从未启用过策略的旧 Worker 保持旧手动行为并显示需升级；已有启用策略的机器不能靠旧版注册绕过限制，
应升级并重启 Worker，或在每周计划中明确关闭策略后再使用旧手动模式。
槽位运行策略不改变 Clock 的任务提交时间；任务计划与槽位模式是两套独立设置。

工作机状态右侧直接显示主机资源：CPU 使用率、可用内存、工作盘可用空间、
活动任务 / 当前目标并发，以及换页速度、可选 PSI 和服务端策略原因。
磁盘值是执行目录采样路径中最低可用空间对应的文件系统，不是所有磁盘容量之和。
资源时间由 Server 单调时钟计算，界面会标出采样过期、连接未确认、失联和旧版未上报；
历史值不表示实时可用量，缺失值显示 —，不会伪装成 0。
CPU 高只表示暂不扩容，不等于机器故障。活动任务超出新目标时等待自然完成，不强杀。
本次资源展示需升级 Server 和 GUI；已升级 v2.5/v2.6 的 Worker 不需再次更新。

### 每日回归开关与白天/每周计划

在 GUI 的“任务管理”右上角打开“定时计划 / 每日开关”。
“开启每日回归”控制原有 crontab 调用的 Clock 是否开始新一轮提交，默认开启。
关闭不会取消已提交任务，也不会打断已被 Server 接受的本轮提交。
原来的 crontab、Tasks.yaml 和每日模板仍照常使用，不必改定时入口。
Clock 的 --dry-run 仍可预览；--check-only 仍可查询。白天/人工任务不会挡住每日回归，
但上一轮每日任务未结束时仍不启动下一轮。

同一窗口可新增、编辑、启停或删除独立的每周计划，选择星期、时间、Server 模板、
相对于 test2 的目录范围（. 表示全部）、ZIP 版本或 latest、优先级及单例超时。
目录会递归发现 run.tcl，模板内容和 ZIP 在每轮触发时固定为不可变快照。
每周计划由 C++ Server 自动触发，不需要另写 crontab、安装 Python 定时库或在 Worker 配置。
每个计划每天最多一次，按 Server 本地时间执行；触发后 5 分钟内可补启动，
超出此窗口的停机漏跑不补历史。首次部署默认没有每周计划，不会自动增加任务。
每轮最多 20000 个案例，最多 32 条计划；范围过大应拆分。

关闭计划会暂停尚未完成的提交及未来轮次，不取消已入队/运行任务；
在任务管理中取消对应批次则停止该轮剩余提交，运行中的 testcase 正常结束。
提交中不能改写或删除该计划，只能暂停/恢复；提交完后可以正常编辑、删除规则。
Server 重启会继续已保存的提交快照，以稳定 submission_key 防止重复。
设置、开关和执行回执存于现有 SQLite；多人修改采用版本检查，不会静默覆盖。
“最近提交”表示入队进度，不是测试通过率；具体执行结果仍在任务管理查看。
每周计划来源为 plan，与 scheduled 每日回归分开，不参加每日回归对比。

GUI 左侧“计划中心”集中管理每日开关和每周计划；支持搜索、状态筛选与逐行启停。
列表表头与保存区固定，编辑抽屉只滚动中间表单，顶部关闭与底部确认按钮不随内容滚走。
新建 / 编辑从右侧抽屉打开，目录树、阶段模板和 ZIP 版本均取自 Server。
所选模板的非零 flow_config 项只读展示，切换模板即时更新；悬停可查看包括关闭项的完整配置。
这里只预览当前模板，不开放自定义阶段，每轮触发时仍读取并固定当时的 Server 模板。
新增计划默认使用 testcase 清单，可直接粘贴或导入任意扩展名的 UTF-8 文本，每行一个相对于 test2 的 run.tcl。
与添加任务使用同一逐行解析规则；空行忽略、重复项去重、保留输入顺序，未知路径报行号，导入失败不覆盖原内容。
仍可切换为目录范围，旧目录计划无需转换。清单触发前由 Server 全量核对，任一案例缺失则该轮失败，不会提交一半。
每个计划最多 20000 个案例，所有计划设置合计最多 512 KiB；清单功能需要同时更新 Server 和 GUI。
修改先加入当前窗口的草稿，点击“保存更改”并收到 Server 确认后才生效；切换页面不会丢草稿。
多人修改冲突或保存结果不确定时禁止再次覆盖，需重新载入核对。

工作机页面的“已下线”只表示暂停调度，连接栏单独显示心跳是否正常；真正断连显示“失联”。
工作状态用蓝色表示已上线、灰色表示已下线、橙色表示下线中/维护/待确认、红色表示失联/失败。
资源卡片默认紧凑显示，换页和 PSI 信息通过“详细指标”展开；采样过期不会伪装成实时状态。
每行“故障恢复”仅在该 slot 失联且有未完成任务时显示；整机入口始终保留，满足条件才可操作。

批次详情可复测筛选中的失败/超时案例。确认窗口默认使用各自原 ZIP 版本，也可明确选择
服务端列出的其他版本；原版本不在可用列表中时不能直接提交，不会自动改用最新版本。
复测保留原执行配置，生成独立人工批次，不覆盖历史、不参与每日回归比较。

正常 GUI/CLI 短连接和底层连接状态仅在 DEBUG 级别记录；Worker 上下线、任务生命周期和异常仍保留。
INFO 默认每 60 秒输出一条合并统计，可通过 `log.metrics_interval_seconds` 调整（5～3600 秒）。
`resp_queued` 表示响应已进入发送队列，不表示对端已收到；`bytes_out` 表示实际写入 socket 的字节数。
连接关闭记录包含进程内唯一 connection_id、远端地址、角色、Worker ID（如有）和关闭原因。

每日对比默认使用 Server 进程内快照缓存，不需要 Redis 或第二套数据库。
翻页、搜索和分类筛选复用快照；相关日期的任务变化或删除会使它失效。
配置位于 server.conf 的 cache.daily.*：默认最多 8 份、64 MiB 预估缓存计费，
活动结果 TTL 5 秒、已结束结果 300 秒。这个预算不是整个 Server 的内存上限。
历史查询使用独立只读连接和限额队列，过宽查询会提示缩小范围；任务状态仍
先同步落库。可用 gcrp_client status 查看 regression_cache 下的耗时、锁等待、
读取条数、命中率计数和预估占用。新索引与查询版本元数据启动时自动创建。

- 本项目暂未提供身份认证，仅部署在受信任内网，不要暴露公网。
- 升级前备份，保留原数据库、Clock 状态、Worker 恢复记录及待上传日志；不要用空目录覆盖运行数据。
- Server/Worker 日志默认保留 30 天；Worker 日志确认已完整上传 Server 后才清理。
- 部署源码不包含数据库、SVN 副本、测试脚本或编译产物。

## 自适应超时（观察版默认设置）

更新 Server、Worker 和 GUI 后，Server 默认使用 timeout.mode=observe。
任务继续使用模板/手动指定的固定超时，同时在派发前记录建议值。
批次详情显示“耗时 / 超时”，单例详情显示实际耗时、本次超时、建议及回退原因。
旧历史没有执行计时，显示“未采集”，不改历史 PASS/FAIL/TIMEOUT。

本次 SQLite 增量升级到 schema 7。升级前先停止 Server 并保留一致性数据库备份；
保留 database、logs、Worker 输出/执行副本和原生产配置。
旧 Server 不支持 schema 7；关闭 timeout.mode 并不能降级数据库。

timeout.mode=off 关闭建议；observe 只观察；enforce 仅对显式 auto 且验证达标的任务缩短。
GUI 高级设置的“允许自动估算超时”默认关闭。
CLI 使用 --timeout-mode auto；Tasks.yaml/模板可选填 timeout_mode: auto；
计划中心的高级设置也可选。原 timeout_seconds/max_time 继续作为固定/回退预算。

真实 Vivado 日志可通过 server.vivado_baseline_root 指定。
运行 python3 -B tools/import_vivado_runtime.py --dry-run 检查；
确认后去掉 --dry-run 生成 database/vivado-runtime-index.json。
解析器只接受明确的“命令: Time (s): cpu = ... ; elapsed = HH:MM:SS”完成记录。
多个主要操作、重复记录、缺失时间会跳过，保留已有索引；
真实内网日志格式尚需用样例核验。没有基准索引时仍可使用历史耗时建议。

建议值需要积累新 Worker 的单调时钟计时，Server 启动和每 30 秒刷新有界统计。
自动执行需成功样本、本机样本、当前版本样本、至少 3 批的观察验证和有效资源遥测。
自动缩短后若发生 TIMEOUT，会持久化保护并恢复固定超时，Server 重启不会清掉保护。
第一版不自动重跑、不自动提高模板预算、不在运行中缩短期限。

保护记录可用 python3 -B tools/timeout_guards.py 查看。
检查触发原因后，停止 Server，再使用 --clear 完整KEY --confirm 清除指定保护；
工具会先保存独立数据库备份。重启 Server 后才重新评估，不会自动重跑旧任务。
修改 timeout.mode 等超时配置后需重启 Server；正在恢复的 run 继续使用原决策。
