# GalaxCore Regression Platform — 干净部署版

基于源码提交 c1a3b957f4bb481c4cca388566eaf9c160546cef 整理。
这是独立部署副本，不包含 Git 历史、测试源码、自动验收脚本、旧数据库、
旧日志、Worker 身份缓存、恢复记录、SVN 副本或旧编译产物。
GUI 和 Worker 的模拟运行入口已移除。

## 目录

- server/：C++ 服务端、Tasks.yaml、flow 模板、clock.py、启停脚本。
- worker/：Python 工作机、slot 管理与完整 GalaxCore SVN 工作副本准备。
- gui/qt/：真实连接的 Qt5 GUI。
- client/：正式 gcrp_client 命令行客户端。
- common/、third_party/：公共代码和必要依赖。
- config/server.conf、config/worker.config：唯一两份部署配置。
- tools/：仅保留服务管理、日志轮转、数据库备份、存储健康检查。
- database/、logs/、worker-output/、worker/workers_slots/：初始为空。

数据库首次启动时由 Server 自动创建（schema v6）；任务 ID 从 1 开始。
已有 v5 数据库可自动升级；升级后不要用旧版本 Server 打开，请先备份。
没有把旧 DB、WAL/SHM、Clock 提交计划或待上传日志一起打包。
项目内的“test2”“PJtest”“testcase”属于真实业务，不是应删除的测试框架。

## 部署前必须做

1. 停止旧 Clock 提交，等待正在执行的案例与日志上报完成。
2. 使用旧项目的 stop.sh 正常停止 Worker 和 Server，备份整个旧部署。
3. **在全新目录解压本包，不要覆盖旧目录。** 不拷入旧数据库、Clock 状态、
   Worker-output、身份文件、活动恢复记录或旧 build。
   新 Server 不能接收带旧待报任务的 Worker，不能把新空 DB 当作重启恢复。
4. 本包已按之前提供的信息预填 192.168.10.11:8080、共享 ZIP 路径和 SVN URL；
   部署前核对实际环境。Server 的 work_root、Tasks.yaml 中 runlist 及其绝对
   路径条目必须对应当前 Server 上的 test2；原有业务模板与每日五条任务保留。
5. 各 Worker 仅按本机容量调整 worker.slots，主机名与 slot1..slotN 自动生成。
   worker.svn_revision 留空；不要填 ZIP 目录。flow_config 由 Server 随任务下发。

## 编译和启动

需要 C++17 编译器、CMake >=3.15.2、SQLite3 开发头文件/库和 pthread。
GUI 额外需要 Qt5 >=5.8 Widgets/Network；Worker 不需要编译 C++。
Python 需要 3.6.8 或以上，Clock 还需要 PyYAML；SVN/Bash 与实际 PJtest 环境
仍由工作机准备。本包不提供 GalaxCore/Vivado 或平台相关预编译二进制。

在项目根目录：

    chmod +x server/*.sh worker/*.sh
    make GUI=OFF
    ./server/start.sh --config config/server.conf
    ./server/status.sh --config config/server.conf

工作机上：

    ./worker/start.sh --config config/worker.config
    ./worker/status.sh --config config/worker.config

普通 make 默认使用 32 并行编译；小机器可加 PARALLEL_JOBS=1。
Qt 主机可 make GUI=ON；GUI 启动只需要：

    ./build/bin/gcrp_gui --ip 192.168.10.11 --port 8080

CLI 查询（明确指定新配置，避免继承旧 GCRP_CONFIG）：

    env GCRP_CONFIG=./config/server.conf ./build/bin/gcrp_client status
    env GCRP_CONFIG=./config/server.conf ./build/bin/gcrp_client workers

## 每日回归

先在 Server 项目根目录检查配置与路径：

    env GCRP_CONFIG=./config/server.conf python3 -B server/clock.py --dry-run

确认后再添加 **一条** crontab；替换下面的项目绝对路径：

    0 10 * * * cd /你的新部署路径/GalaxCoreRegressionPlatform && /usr/bin/env GCRP_CONFIG=./config/server.conf /usr/bin/python3 -B server/clock.py >/dev/null 2>&1

不要同时保留旧 Clock 定时项。每条 Tasks.yaml 是一个批次，整批提交后等待
30 秒，不是每个 testcase 等待。一天只发布一份不可变计划；故障后保留并
续用新部署生成的 Clock 状态，不要删除状态强行再提交。

## 日常运维

    python3 -B tools/health_check.py --role server
    python3 -B tools/health_check.py --role worker
    python3 -B tools/backup_database.py
    ./worker/reload.sh --config config/worker.config
    ./worker/stop.sh --config config/worker.config
    ./server/stop.sh --config config/server.conf

运行目录会在使用时自动创建。Server/Worker 日志默认保留 30 天；
Worker 的任务日志只有在 Server 完整文件集校验确认后才清理，失败则保留。
源 testcase 的原始 run、runner 日志与业务结果应区分查看。
单个归档文件上限 1 GiB，超过需人工转移，不能删除待报证据强行恢复。

Clock 每日提交前备份 SQLite，默认保留 7 天；同机备份不是异地灾备，
应另行复制已校验的备份。共享 ZIP 的生成与最近 50 版清理由原维护流程负责。

人工复测新建人工批次，不覆盖历史，不参与每日回归比较。取消只处理未开始
和后续案例，ASSIGNED/RUNNING 正常结束。
添加任务时，业务目标位于设置区顶部；批次名称自动使用“人工验证 + 所选模板名”，
切换模板同步更新。同名批次仍生成独立 UUID，不会合并；高级设置仅含优先级与超时。

本版按部署要求开放远程管理，不提供身份认证：可连接 Server 的用户都能
暂停/恢复接单、申请 SVN 更新和删除已结束记录。只能部署在受信任内网，
通过防火墙限制可访问机器，不得暴露公网。恢复整机接单只恢复 ONLINE slots；
SVN 更新需 Worker 支持维护协议，排空后执行，不会强杀正在运行的案例。
GUI 将暂停/恢复接单标为“下线/上线”，并把 SVN 更新标为“更新”；这些操作
不改变通信状态 ONLINE/OFFLINE，也不远程关机。Slot 操作按钮最小宽度 112，
操作列至少 260，窄窗口使用横向滚动，不再挤掉文字。

任务管理支持删除已结束批次；单 testcase 可在详情中删除。活动任务由 Server
强制拒绝，需先停止后续并等待运行任务结束。采用持久化逻辑删除，记录从
任务/回归查询消失，重启不恢复；原始日志、防重复提交键、ID 高水位和不可变
每日计划保留。每日计划会标明 DELETED，不把人工删除误认为 Clock 未提交。
删除不是磁盘清理，也不会删除共享 GalaxCore ZIP。GUI 删除需要二次确认。

## 清理边界

原开发仓库和原运行数据没有被删除，作为回退备份保留。
部署包内已删除测试/验收代码及构建入口；不得使用本包声称替代完整回归验证。
清单 DEPLOYMENT-MANIFEST.json 提供来源与逐文件 SHA-256（不包含清单自身）。
