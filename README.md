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

## 注意

- 本项目暂未提供身份认证，仅部署在受信任内网，不要暴露公网。
- 升级前备份，保留原数据库、Clock 状态、Worker 恢复记录及待上传日志；不要用空目录覆盖运行数据。
- Server/Worker 日志默认保留 30 天；Worker 日志确认已完整上传 Server 后才清理。
- 部署源码不包含数据库、SVN 副本、测试脚本或编译产物。
