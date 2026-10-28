#include "ConsoleSupport.h"
#include <QTimeEdit>
#include <QScrollBar>

namespace {
Json runPolicy(const Json& worker) {
    return worker.contains("run_policy") && worker["run_policy"].is_object() ? worker["run_policy"]
                                                                             : Json::object();
}

QString policyMode(const QString& mode) {
    return mode == "auto"   ? QStringLiteral("自动")
           : mode == "half" ? QStringLiteral("半开")
           : mode == "full" ? QStringLiteral("全开")
                            : QStringLiteral("未提供");
}

QString policyReason(const Json& policy) {
    const auto reason = text(policy, "reason");
    const QMap<QString, QString> reasons{{"upgrade_worker", QStringLiteral("请升级 Worker")},
        {"telemetry_stale", QStringLiteral("资源采样过期")},
        {"telemetry_missing", QStringLiteral("尚未收到资源采样")},
        {"disk_low", QStringLiteral("磁盘可用空间不足")},
        {"memory_critical", QStringLiteral("内存严重不足")},
        {"adaptive_disabled", QStringLiteral("自适应资源保护已关闭")},
        {"memory_pressure", QStringLiteral("内存压力偏高")},
        {"swap_pressure", QStringLiteral("交换空间压力偏高")},
        {"memory_stall", QStringLiteral("内存访问阻塞")},
        {"io_pressure", QStringLiteral("磁盘读写压力偏高")},
        {"cpu_warmup", QStringLiteral("CPU 负载预热观察中")},
        {"cpu_busy", QStringLiteral("CPU 繁忙")},
        {"gradual_recovery", QStringLiteral("资源恢复中，逐步增加槽位")},
        {"cpu_high", QStringLiteral("CPU 使用率偏高")},
        {"memory_low", QStringLiteral("可用内存不足")},
        {"resource_pressure", QStringLiteral("资源压力保护")},
        {"resource_waiting", QStringLiteral("等待资源采样")},
        {"resource_stale", QStringLiteral("资源采样过期")},
        {"resource_invalid", QStringLiteral("资源采样无效")},
        {"workday", QStringLiteral("工作日时段")},
        {"workday_half", QStringLiteral("工作日半开时段")},
        {"off_hours", QStringLiteral("非工作时段")},
        {"weekend", QStringLiteral("周末全开")},
        {"manual_full", QStringLiteral("临时全开")},
        {"manual_half", QStringLiteral("临时半开")},
        {"override", QStringLiteral("临时模式生效")},
        {"disabled", QStringLiteral("计划已关闭")},
        {"legacy", QStringLiteral("旧版 Worker 未提供资源信息")},
        {"normal", QStringLiteral("资源正常")}};
    if (reasons.contains(reason))
        return reasons.value(reason);
    const auto resource = text(policy, "resource_status");
    if (resource == "waiting")
        return QStringLiteral("等待资源采样");
    if (resource == "stale")
        return QStringLiteral("资源采样过期");
    if (resource == "invalid")
        return QStringLiteral("资源采样无效");
    if (resource == "legacy")
        return QStringLiteral("旧版 Worker 未提供资源信息");
    return reason == "—" || reason.isEmpty() ? QStringLiteral("未提供原因")
                                             : QStringLiteral("服务端策略原因：%1").arg(reason);
}

uint64_t occupiedSlots(const Json& worker) {
    return qMax(
        uint64_t(worker.value("used", 0)), worker.value("current_tasks_total", uint64_t(0)));
}

bool dispatchEnabled(const Json& worker) {
    return text(worker, "status") == "ONLINE" && worker.value("accepting_tasks", true);
}

QString workState(const Json& worker, bool stale) {
    if (stale)
        return QStringLiteral("状态未确认 · 数据过期");
    const auto connection = text(worker, "status");
    const bool busy = occupiedSlots(worker) > 0;
    if (connection == "OFFLINE")
        return busy ? QStringLiteral("失联 · 执行待确认") : QStringLiteral("失联 · 连接中断");
    if (connection != "ONLINE" && connection != "DRAINING")
        return QStringLiteral("状态未确认");
    if (worker.contains("current_tasks") && worker["current_tasks"].is_array())
        for (const auto& task : worker["current_tasks"])
            if (task.value("recovery_required", false))
                return QStringLiteral("状态未确认 · 待恢复");
    const auto policy = runPolicy(worker);
    const auto phase = text(worker.value("maintenance", Json::object()), "state");
    if (phase == "PENDING")
        return busy ? QStringLiteral("下线中 · 完成后更新") : QStringLiteral("维护中 · 等待更新");
    if (phase == "RUNNING")
        return QStringLiteral("维护中 · SVN 更新");
    if (phase == "FAILED")
        return QStringLiteral("已下线 · 更新失败");
    if (connection == "DRAINING")
        return busy ? QStringLiteral("下线中 · 当前案例继续")
                    : QStringLiteral("已下线 · 服务退出中");
    if (!worker.value("accepting_requested", worker.value("accepting_tasks", true)))
        return busy ? QStringLiteral("下线中 · 当前案例继续") : QStringLiteral("已下线 · 人工暂停");
    if (!policy.empty()) {
        const bool allowed = policy.value("slot_allowed", true),
                   resume = policy.value("resume_allowed", true);
        if (busy && (!allowed || !resume))
            return QStringLiteral("下线中 · 当前案例继续");
        if (!resume)
            return QStringLiteral("已下线 · 模式限制");
        if (!allowed)
            return QStringLiteral("已下线 · 资源保护");
    }
    if (!dispatchEnabled(worker))
        return busy ? QStringLiteral("下线中 · 当前案例继续") : QStringLiteral("已下线 · 暂停调度");
    if (worker.contains("current_tasks") && worker["current_tasks"].is_array())
        for (const auto& task : worker["current_tasks"]) {
            if (text(task, "status") == "RUNNING")
                return QStringLiteral("已上线 · 执行中");
        }
    return busy ? QStringLiteral("已上线 · 准备执行") : QStringLiteral("已上线 · 等待任务");
}
} // namespace

void MainWindow::showWorkers() {
    const bool sameHost = hostTitle_->text() == machines_->currentText();
    const int scrollY = sameHost ? slotsTable_->verticalScrollBar()->value() : 0,
              scrollX = sameHost ? slotsTable_->horizontalScrollBar()->value() : 0;
    const int hostsY = hostsList_->verticalScrollBar()->value();
    const auto selectedWorker =
        slotsTable_->currentRow() >= 0 && slotsTable_->item(slotsTable_->currentRow(), 0)
            ? slotsTable_->item(slotsTable_->currentRow(), 0)->text()
            : QString();
    const auto selected = machines_->currentText();
    QSet<QString> machineNames;
    const auto visible = [this](const Json& w) {
        return showOffline_->isChecked() || text(w, "status") != "OFFLINE" || occupiedSlots(w) > 0;
    };
    for (const auto& w : workers_)
        if (visible(w))
            machineNames.insert(text(w, "machine_id"));
    {
        QSignalBlocker blocked(machines_);
        machines_->clear();
        auto list = machineNames.values();
        list.sort();
        machines_->addItems(list);
        if (list.contains(selected))
            machines_->setCurrentText(selected);
    }
    {
        QSignalBlocker blocked(hostsList_);
        hostsList_->clear();
        for (int i = 0; i < machines_->count(); ++i) {
            const auto name = machines_->itemText(i);
            int slotCount = 0, busy = 0, enabled = 0;
            bool connected = false, maintenance = false;
            for (const auto& w : workers_)
                if (text(w, "machine_id") == name && visible(w)) {
                    ++slotCount;
                    busy += occupiedSlots(w) > 0;
                    enabled += dispatchEnabled(w);
                    connected = connected || text(w, "status") == "ONLINE" ||
                                text(w, "status") == "DRAINING";
                    const auto phase = text(w.value("maintenance", Json::object()), "state");
                    maintenance = maintenance || phase == "PENDING" || phase == "RUNNING";
                }
            auto item = new QListWidgetItem(hostsList_);
            item->setData(Qt::UserRole, name);
            item->setSizeHint(QSize(180, 90));
            const auto status =
                workersStale_ && !demo_ ? QStringLiteral("？ 数据过期")
                : enabled ? QStringLiteral("<span style='color:#3b82f6'>●</span> 已上线")
                : !connected && busy
                    ? QStringLiteral("<span style='color:#b58220'>●</span> 失联待处理")
                : busy || maintenance
                    ? QStringLiteral("<span style='color:#b58220'>●</span> 下线中 / 维护中")
                : connected ? QStringLiteral("<span style='color:#94a3b8'>●</span> 已下线")
                            : QStringLiteral("<span style='color:#94a3b8'>●</span> 连接中断");
            auto label = new QLabel(name.toHtmlEscaped() +
                                    "<br><span style='color:#8493a8;font-size:11px'>" + status +
                                    QStringLiteral("<br>%1 slots · %2 已上线 · %3 执行/准备</span>")
                                        .arg(slotCount)
                                        .arg(enabled)
                                        .arg(busy));
            label->setContentsMargins(14, 8, 8, 8);
            label->setAttribute(Qt::WA_TransparentForMouseEvents);
            hostsList_->setItemWidget(item, label);
        }
        hostsList_->setCurrentRow(machines_->currentIndex());
        hostsList_->doItemsLayout();
        hostsList_->verticalScrollBar()->setValue(hostsY);
    }
    slotsTable_->setRowCount(0);
    slotsTable_->setColumnWidth(4, 200);
    hostTitle_->setText(machines_->currentText().isEmpty() ? QStringLiteral("暂无在线工作机")
                                                           : machines_->currentText());
    bool anyOnline = false, anyRecoverable = false;
    int onlineCount = 0, offlineCount = 0, legacyCount = 0, enabledCount = 0, busyCount = 0;
    for (const auto& w : workers_)
        if (text(w, "machine_id") == machines_->currentText()) {
            if (text(w, "status") == "OFFLINE")
                ++offlineCount;
            else
                ++onlineCount;
            enabledCount += dispatchEnabled(w);
            busyCount += occupiedSlots(w) > 0;
            if (!w.value("control_supported", false) && text(w, "status") != "OFFLINE")
                ++legacyCount;
            if (!visible(w))
                continue;
            int row = slotsTable_->rowCount();
            slotsTable_->insertRow(row);
            cell(slotsTable_, row, 0, text(w, "slot_id") + "\n" + text(w, "worker_id"));
            if (slotsTable_->item(row, 0)->text() == selectedWorker)
                slotsTable_->setCurrentCell(row, 0);
            const auto status = workState(w, !demo_ && workersStale_);
            cell(slotsTable_, row, 1, status);
            const bool failed =
                status.contains(QStringLiteral("失联")) || status.contains(QStringLiteral("失败"));
            const bool warning = status.contains(QStringLiteral("下线中")) ||
                                 status.contains(QStringLiteral("维护中")) ||
                                 status.contains(QStringLiteral("未确认"));
            const bool ready = status.startsWith(QStringLiteral("已上线"));
            slotsTable_->item(row, 1)->setForeground(QColor(failed    ? "#b44058"
                                                            : warning ? "#987022"
                                                            : ready   ? "#2865d2"
                                                                      : "#72829a"));
            slotsTable_->item(row, 1)->setBackground(QColor(failed    ? "#fff0f2"
                                                            : warning ? "#fff8e8"
                                                            : ready   ? "#edf4ff"
                                                                      : "#f3f5f8"));
            auto stateFont = slotsTable_->item(row, 1)->font();
            stateFont.setBold(true);
            slotsTable_->item(row, 1)->setFont(stateFont);
            slotsTable_->item(row, 1)->setToolTip(QStringLiteral(
                "已下线表示暂停调度，不是通信失联。连接是否正常由右侧心跳状态单独说明。"));
            const bool online = text(w, "status") == "ONLINE";
            anyOnline = anyOnline || online;
            if (!demo_ && !w.value("control_supported", false))
                slotsTable_->item(row, 0)->setToolTip(QStringLiteral(
                    "此进程未声明维护协议 V1，可能为旧版 Worker；槽位来自 Server 注册/心跳，不按磁盘目录推算。"));
            if (!w.value("registration_observed", true))
                slotsTable_->item(row, 0)->setToolTip(QStringLiteral(
                    "Server 重启后保留的未确认执行。本进程尚未收到该 Worker 注册；暂按原 Worker ID 单独列出，不推测主机名。"));
            const bool accepting = w.value("accepting_tasks", true),
                       requested = w.value("accepting_requested", accepting);
            const auto policy = runPolicy(w);
            const auto maintenance = w.value("maintenance", Json::object());
            const auto phase =
                QString::fromStdString(maintenance.value("state", std::string("IDLE")));
            const bool pending = phase == "PENDING" || phase == "RUNNING";
            const auto connection = text(w, "status");
            cell(slotsTable_,
                row,
                2,
                QStringLiteral("连接：") +
                    (connection == "ONLINE"        ? QStringLiteral("已连接 · 心跳正常")
                        : connection == "DRAINING" ? QStringLiteral("已连接 · 服务退出中")
                        : connection == "OFFLINE"  ? QStringLiteral("中断 · 等待重连")
                                                   : QStringLiteral("未确认")) +
                    "\n" + QStringLiteral("调度：") + status);
            if (workersStale_ && !demo_)
                slotsTable_->item(row, 2)->setText(
                    QStringLiteral("上次快照 · ") + slotsTable_->item(row, 2)->text());
            const auto maintenanceText = phase == "PENDING"
                                             ? QStringLiteral("等待当前案例完成后更新")
                                         : phase == "RUNNING" ? QStringLiteral("SVN 更新中")
                                         : phase == "FAILED" ? QStringLiteral("更新失败 / 保持下线")
                                         : phase == "SUCCEEDED" ? QStringLiteral("更新完成")
                                                                : QString{};
            if (!maintenanceText.isEmpty())
                slotsTable_->item(row, 2)->setText(
                    slotsTable_->item(row, 2)->text() + "\n" + maintenanceText);
            slotsTable_->item(row, 2)->setToolTip(QStringLiteral("原始连接状态：") + connection +
                                                  "\n" +
                                                  QString::fromStdString(maintenance.dump(2)));
            auto controls = new QWidget;
            auto actions = new QHBoxLayout(controls);
            actions->setContentsMargins(6, 2, 6, 2);
            actions->setSpacing(8);
            const auto worker = text(w, "worker_id");
            const bool resumeAllowed = policy.value("resume_allowed", true);
            const bool pauseAction = requested && (resumeAllowed || occupiedSlots(w) > 0);
            auto toggle =
                new QPushButton(pauseAction ? QStringLiteral("下线") : QStringLiteral("上线"));
            toggle->setObjectName("slotAcceptButton");
            auto update = new QPushButton(QStringLiteral("更新"));
            update->setObjectName("slotUpdateButton");
            actions->addWidget(toggle);
            actions->addWidget(update);
            auto recover = new QPushButton(QStringLiteral("故障恢复…"));
            recover->setObjectName("slotRecoveryButton");
            actions->addWidget(recover);
            const bool recoverable =
                text(w, "status") == "OFFLINE" && w.value("current_tasks_total", uint64_t(0)) > 0;
            recover->setVisible(recoverable);
            // Cell widgets are not reliably included by ResizeToContents on older Qt5.
            // Reserve both button widths explicitly; a narrow window scrolls instead of clipping.
            for (auto button : {toggle, update})
                button->setMinimumWidth(qMax(64, button->sizeHint().width()));
            recover->setMinimumWidth(qMax(96, recover->sizeHint().width()));
            controls->setMinimumWidth(actions->minimumSize().width());
            slotsTable_->setColumnWidth(
                4, qMax(slotsTable_->columnWidth(4), controls->minimumWidth() + 16));
            const bool enabled =
                (demo_ || (!workersStale_ && operations_.contains("control_workers"))) &&
                !managementPending_ && online;
            toggle->setToolTip(QStringLiteral(
                "上线/下线只改变调度资格，不断开连接，也不终止正在运行的 testcase。"));
            update->setToolTip(
                QStringLiteral("排空当前任务后更新独立 SVN 副本；需要新版 Worker 支持。"));
            toggle->setEnabled(enabled && (pauseAction || (!pending && resumeAllowed)));
            if (!resumeAllowed)
                toggle->setToolTip(
                    QStringLiteral(
                        "当前模式或资源保护不允许上线；运行中的任务可以手动下线并等待完成。") +
                    policyReason(policy));
            update->setEnabled(
                enabled && !pending && (demo_ || w.value("control_supported", false)));
            anyRecoverable = anyRecoverable || recoverable;
            recover->setEnabled(!demo_ && !workersStale_ && !managementPending_ &&
                                operations_.contains("recover_offline_workers") && recoverable);
            recover->setToolTip(QStringLiteral(
                "仅处理失联 slot 的未完成任务；必须先确认原执行进程已停止。不是远程关机，也不会自动重新上线。"));
            connect(toggle, &QPushButton::clicked, this, [this, worker, pauseAction] {
                controlWorkers(false, worker, pauseAction ? "pause" : "resume");
            });
            connect(update, &QPushButton::clicked, this, [this, worker] {
                controlWorkers(false, worker, "update");
            });
            connect(recover, &QPushButton::clicked, this, [this, worker] {
                recoverOfflineWorkers(false, worker);
            });
            slotsTable_->setCellWidget(row, 4, controls);
            QStringList execution;
            QString firstTask;
            if (w.contains("current_tasks") && w["current_tasks"].is_array()) {
                for (const auto& task : w["current_tasks"]) {
                    if (firstTask.isEmpty())
                        firstTask = text(task, "task_id");
                    auto phase =
                        task.value("recovery_required", false) || text(w, "status") == "OFFLINE"
                            ? QStringLiteral("待确认")
                        : text(task, "status") == "RUNNING" ? QStringLiteral("执行中")
                                                            : QStringLiteral("待启动");
                    auto path = text(task, "case");
                    if (path.isEmpty() || path == "—")
                        path = QStringLiteral("未提供 testcase 路径");
                    execution << phase + " · Task #" + text(task, "task_id") + "\n" + path;
                }
                if (w.value("current_tasks_truncated", false))
                    execution << QStringLiteral("另有活动任务，详见任务查询；本列最多显示 8 条。");
            } else
                execution << QStringLiteral("Server 未提供当前任务明细，请升级 Server。");
            if (execution.isEmpty())
                execution << (occupiedSlots(w) > 0 ? QStringLiteral("任务状态待确认，请刷新。")
                              : text(w, "status") == "OFFLINE"
                                  ? QStringLiteral("连接中断，无可确认的运行案例")
                              : accepting ? QStringLiteral("等待任务")
                                          : QStringLiteral("当前无执行任务"));
            if (workersStale_ && !demo_)
                execution.prepend(QStringLiteral("上次任务快照，当前执行状态未确认"));
            cell(slotsTable_, row, 3, execution.join("\n\n"));
            slotsTable_->item(row, 3)->setToolTip(execution.join("\n\n"));
            if (!firstTask.isEmpty())
                slotsTable_->item(row, 3)->setData(Qt::UserRole, firstTask);
            const auto details = execution.join("\n\n");
            const int lines = details.count('\n') + 1;
            slotsTable_->setRowHeight(
                row, qBound(76, lines * slotsTable_->fontMetrics().lineSpacing() + 20, 180));
        }
    slotsTable_->doItemsLayout();
    slotsTable_->verticalScrollBar()->setValue(scrollY);
    slotsTable_->horizontalScrollBar()->setValue(scrollX);
    workerSummary_->setText(
        (demo_              ? QStringLiteral("【演示数据】 ")
            : workersStale_ ? QStringLiteral("【过期快照】 ")
                            : QStringLiteral("【Server 注册信息】 ")) +
        QStringLiteral(
            "%1：已连接/退出中 %2，连接中断 %3，已上线 %4，执行/准备 %5；%6 个已连接 slot 未声明维护协议。")
            .arg(machines_->currentText())
            .arg(onlineCount)
            .arg(offlineCount)
            .arg(enabledCount)
            .arg(busyCount)
            .arg(legacyCount));
    if (machines_->count() == 0)
        workerSummary_->setText(
            workersStale_
                ? QStringLiteral("尚未确认 Server 工作机信息，请检查连接。")
                : QStringLiteral(
                      "当前没有在线工作机。可勾选查看离线历史 slots；不会用演示数据填充。"));
    for (auto button : hostControlButtons_) {
        button->setEnabled((demo_ || (!workersStale_ && operations_.contains("control_workers"))) &&
                           anyOnline && !managementPending_);
        button->setToolTip(
            QStringLiteral("整机操作仅针对当前已连接 slots；全部上线不远程启动失联机器。"));
    }
    hostRecoveryButton_->setEnabled(!demo_ && !workersStale_ && !managementPending_ &&
                                    operations_.contains("recover_offline_workers") &&
                                    anyRecoverable);
    hostRecoveryButton_->setToolTip(
        QStringLiteral("只处理所选服务器中失联且有未完成任务的 slots，已连接 slots 不受影响。"));
    Json policy = Json::object();
    for (const auto& w : workers_)
        if (text(w, "machine_id") == machines_->currentText()) {
            policy = runPolicy(w);
            break;
        }
    const bool supported = policy.value("supported", false);
    for (auto button : hostPolicyButtons_) {
        const bool canEdit = supported || (button->objectName() == "hostPolicyPlan" &&
                                              policy.value("enabled", false));
        button->setEnabled(canEdit && anyOnline && !workersStale_ && !managementPending_ &&
                           operations_.contains("control_workers"));
        button->setToolTip(
            supported
                ? QStringLiteral(
                      "半开 / 全开默认临时生效 4 小时；Auto 恢复每周计划。模式切换不会自动上线手动下线的槽位。")
                : QStringLiteral("请升级 Worker 和 Server，以支持主机运行策略。"));
    }
    if (policy.empty())
        hostPolicySummary_->setText(
            QStringLiteral("Server 未提供运行策略；请升级 Server / Worker。"));
    else if (!supported)
        hostPolicySummary_->setText(
            QStringLiteral("此主机不支持运行策略，请升级 Worker / Server。"));
    else {
        const QString compact =
            (workersStale_ ? QStringLiteral("【过期快照】 ") : QString()) +
            (policy.value("enabled", false) ? policyMode(text(policy, "effective_mode"))
                                            : QStringLiteral("人工控制")) +
            QStringLiteral(" · 当前运行 %1 / 允许并发 %2")
                .arg(text(policy, "active_tasks"))
                .arg(text(policy, "target_limit")) +
            (policy.value("policy_reload_error", std::string()).empty()
                    ? QString()
                    : QStringLiteral(" · 策略加载失败，沿用原配置"));
        QString expiry = QStringLiteral("无临时覆盖");
        if (policy.value("policy_source", std::string()) == "file")
            expiry = policy.value("policy_reload_error", std::string()).empty()
                         ? QStringLiteral("独立策略文件管理 · 每%1秒重读 · GUI只读")
                               .arg(policy.value("policy_reload_seconds", 30))
                         : QStringLiteral("策略重载失败，沿用上一份有效配置");
        const auto until = policy.value("override_until_ms", int64_t(0));
        if (until > 0)
            expiry = QStringLiteral("临时覆盖至 %1（按本机时区显示）")
                         .arg(QDateTime::fromMSecsSinceEpoch(until).toLocalTime().toString(
                             "yyyy-MM-dd HH:mm t"));
        QString details =
            (workersStale_ ? QStringLiteral("【过期快照】 ") : QString()) +
            QStringLiteral(
                "配置 %1 → 生效 %2 · 模式上限 %3 / 目标 %4 / 注册 %5 · 活动任务 %6\n%7 · %8")
                .arg(policyMode(text(policy, "configured_mode")))
                .arg(policyMode(text(policy, "effective_mode")))
                .arg(text(policy, "mode_limit"))
                .arg(text(policy, "target_limit"))
                .arg(text(policy, "reported_slots"))
                .arg(text(policy, "active_tasks"))
                .arg(policy.value("enabled", false) ? QStringLiteral("策略启用")
                                                    : QStringLiteral("策略关闭"))
                .arg(expiry);
        if (policy.value("policy_source", std::string()) == "file")
            details += QStringLiteral("\n基础并发 %1 · 当前档位 %2 · 当前内存占用上限 %3%")
                           .arg(policy.value("base_limit", 0))
                           .arg(policy.value("current_tier", 0))
                           .arg(policy.value("resource_thresholds", Json::object())
                                    .value("memory_used_max_percent", 0.0),
                               0,
                               'f',
                               1);
        hostPolicySummary_->setProperty("policyCompactText", compact);
        hostPolicySummary_->setProperty("policyDetailText", compact + "\n" + details);
        hostPolicySummary_->setText(hostPolicySummary_->property("policyDetailsExpanded").toBool()
                                        ? compact + "\n" + details
                                        : compact);
        hostPolicySummary_->setToolTip(QString::fromStdString(policy.dump(2)));
    }
    if (policy.empty() || !supported) {
        hostPolicySummary_->setProperty("policyCompactText", hostPolicySummary_->text());
        hostPolicySummary_->setProperty("policyDetailText", hostPolicySummary_->text());
        hostPolicySummary_->setToolTip(QString());
    }
    showHostResources();
}

void MainWindow::setHostPolicy(const QString& machine, const Json& policy) {
    if (machine.isEmpty() || managementPending_ || workersStale_ ||
        !operations_.contains("control_workers"))
        return;
    bool supported = false;
    for (const auto& w : workers_)
        if (text(w, "machine_id") == machine && runPolicy(w).value("supported", false))
            supported = true;
    if (!supported && (!policy.contains("enabled") || policy["enabled"] != false))
        return;
    Json request = {{"machine_id", machine.toStdString()},
        {"action", "policy"},
        {"operation_id", QUuid::createUuid().toString().mid(1, 36).toStdString()},
        {"policy", policy}};
    managementPending_ = true;
    showWorkers();
    rpc_.request("control_workers", request, [this](RpcReply r) {
        managementPending_ = false;
        workersStale_ = true;
        updateFreshness();
        showWorkers();
        // A snapshot may still be loading when control ACK arrives. Do not let
        // refresh() silently skip the new policy and leave stale buttons visible.
        auto retry = findChild<QTimer*>("policySnapshotRetry");
        if (!retry) {
            retry = new QTimer(this);
            retry->setObjectName("policySnapshotRetry");
            retry->setInterval(100);
            connect(retry, &QTimer::timeout, this, [this, retry] {
                if (loading_ || submitting_ || regressionLoading_)
                    return;
                retry->stop();
                refresh();
            });
        }
        retry->start();
        if (!r.ok) {
            note(QStringLiteral("运行策略请求未确认：") + r.error +
                     QStringLiteral("。正在重新核对状态。"),
                true);
            return;
        }
        note(QStringLiteral("运行策略请求已接受；正在刷新，手动下线状态保留。"));
    });
}

void MainWindow::editHostPolicy() {
    const auto machine = machines_->currentText();
    Json policy = Json::object();
    for (const auto& w : workers_)
        if (text(w, "machine_id") == machine) {
            policy = runPolicy(w);
            break;
        }
    if ((!policy.value("supported", false) && !policy.value("enabled", false)) || workersStale_ ||
        managementPending_)
        return;
    const auto plan =
        policy.contains("plan") && policy["plan"].is_object() ? policy["plan"] : Json::object();
    QSignalBlocker blockRefresh(refreshTimer_), blockFreshness(freshnessTimer_);
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("每周计划 · ") + machine);
    dialog.setObjectName("hostPolicyDialog");
    auto box = new QVBoxLayout(&dialog);
    auto hint = new QLabel(QStringLiteral(
        "按 Server 本地时间执行：工作日时段半开，其余时间及周末全开。默认周一至周五 09:00–19:00。模式切换不会自动上线手动下线槽位。"));
    hint->setWordWrap(true);
    box->addWidget(hint);
    auto form = new QFormLayout;
    box->addLayout(form);
    auto mode = new QComboBox;
    mode->addItem(QStringLiteral("Auto · 每周计划"), "auto");
    mode->addItem(QStringLiteral("临时半开"), "half");
    mode->addItem(QStringLiteral("临时全开"), "full");
    mode->setCurrentIndex(qMax(0, mode->findData(text(policy, "configured_mode"))));
    form->addRow(QStringLiteral("模式"), mode);
    auto duration = new QSpinBox;
    duration->setRange(1, 168);
    duration->setValue(4);
    duration->setSuffix(QStringLiteral(" 小时"));
    form->addRow(QStringLiteral("临时模式时长"), duration);
    auto enabled = new QCheckBox(QStringLiteral("启用运行策略（关闭后仅人工控制）"));
    enabled->setChecked(plan.value("enabled", true));
    form->addRow(enabled);
    auto adaptive = new QCheckBox(QStringLiteral("自动收缩 / 恢复并发（严重资源不足仍保护）"));
    adaptive->setChecked(plan.value("adaptive", true));
    form->addRow(adaptive);
    auto days = new QWidget;
    auto daysRow = new QHBoxLayout(days);
    daysRow->setContentsMargins(0, 0, 0, 0);
    QList<QCheckBox*> checks;
    const QStringList names{QStringLiteral("一"),
        QStringLiteral("二"),
        QStringLiteral("三"),
        QStringLiteral("四"),
        QStringLiteral("五"),
        QStringLiteral("六"),
        QStringLiteral("日")};
    const auto workdays = plan.value("workdays", Json::array({1, 2, 3, 4, 5}));
    for (int i = 1; i <= 7; ++i) {
        auto check = new QCheckBox(names[i - 1]);
        check->setChecked(std::find(workdays.begin(), workdays.end(), Json(i)) != workdays.end());
        checks << check;
        daysRow->addWidget(check);
    }
    form->addRow(QStringLiteral("工作日"), days);
    auto start = new QTimeEdit;
    auto end = new QTimeEdit;
    start->setDisplayFormat("HH:mm");
    end->setDisplayFormat("HH:mm");
    start->setTime(QTime(0, 0).addSecs(qBound(0, plan.value("day_start_minute", 540), 1439) * 60));
    end->setTime(QTime(0, 0).addSecs(qBound(0, plan.value("day_end_minute", 1140), 1439) * 60));
    form->addRow(QStringLiteral("半开开始"), start);
    form->addRow(QStringLiteral("半开结束"), end);
    auto half = new QSpinBox;
    half->setRange(0, 128);
    half->setValue(plan.value("half_slots", 0));
    half->setSpecialValueText(QStringLiteral("自动 · 槽位数的一半向上取整"));
    form->addRow(QStringLiteral("半开槽位数"), half);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    box->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (start->time() >= end->time()) {
            QMessageBox::warning(
                &dialog, QStringLiteral("时段无效"), QStringLiteral("开始时间必须早于结束时间。"));
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted)
        return;
    Json selected = Json::array();
    for (int i = 0; i < checks.size(); ++i)
        if (checks[i]->isChecked())
            selected.push_back(i + 1);
    setHostPolicy(machine,
        {{"mode", mode->currentData().toString().toStdString()},
            {"duration_minutes", duration->value() * 60},
            {"workdays", selected},
            {"day_start_minute", start->time().hour() * 60 + start->time().minute()},
            {"day_end_minute", end->time().hour() * 60 + end->time().minute()},
            {"half_slots", half->value()},
            {"adaptive", adaptive->isChecked()},
            {"enabled", enabled->isChecked()}});
}

void MainWindow::recoverOfflineWorkers(bool machine, const QString& target) {
    if (target.isEmpty() || managementPending_ || workersStale_ ||
        !operations_.contains("recover_offline_workers"))
        return;
    Json expected = Json::array();
    QStringList details;
    for (const auto& w : workers_) {
        if (text(w, machine ? "machine_id" : "worker_id") != target ||
            text(w, "status") != "OFFLINE")
            continue;
        if (w.value("current_tasks_truncated", false)) {
            note(
                QStringLiteral("当前任务明细被截断，不能安全确认故障恢复，请联系维护人员。"), true);
            return;
        }
        if (!w.contains("current_tasks") || !w["current_tasks"].is_array())
            continue;
        for (const auto& task : w["current_tasks"]) {
            expected.push_back({{"task_id", task["task_id"]},
                {"run_id", task["run_id"]},
                {"worker_id", w["worker_id"]}});
            details << text(w, "worker_id") + " · Task #" + text(task, "task_id") + "\n" +
                           text(task, "case") + "\nRun: " + text(task, "run_id");
        }
    }
    if (expected.empty() || expected.size() > 256) {
        note(QStringLiteral("没有可恢复的失联任务，或任务数超出单次安全上限（256）；请刷新。"),
            true);
        return;
    }
    QSignalBlocker blockRefresh(refreshTimer_), blockFreshness(freshnessTimer_);
    QDialog dialog(this);
    dialog.setObjectName("offlineRecoveryDialog");
    dialog.setWindowTitle(QStringLiteral("确认故障并重新排队"));
    dialog.resize(700, 480);
    auto layout = new QVBoxLayout(&dialog);
    auto warning = new QLabel(QStringLiteral(
        "目标：%1\n失联不代表执行已停止。断网时原机器上的 GalaxCore 可能仍在运行，重新排队可能造成重复执行。\n此操作不会关闭原进程！旧执行记录将保留，任务从头重跑；已取消批次不重排，超过重试上限的任务转为失败。故障 slot 保持下线。")
                                  .arg(target));
    warning->setTextFormat(Qt::PlainText);
    warning->setWordWrap(true);
    layout->addWidget(warning);
    auto list = new QPlainTextEdit;
    list->setReadOnly(true);
    list->setPlainText(details.join("\n\n"));
    layout->addWidget(list, 1);
    auto confirmed =
        new QCheckBox(QStringLiteral("我已确认原机器已关机，或上述任务的相关执行进程已经停止"));
    confirmed->setObjectName("confirmExecutionStopped");
    layout->addWidget(confirmed);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto apply = buttons->addButton(QStringLiteral("确认并重新排队"), QDialogButtonBox::AcceptRole);
    apply->setObjectName("confirmRecoveryButton");
    apply->setEnabled(false);
    apply->setAutoDefault(false);
    apply->setDefault(false);
    auto cancel = buttons->button(QDialogButtonBox::Cancel);
    cancel->setDefault(true);
    cancel->setFocus();
    layout->addWidget(buttons);
    connect(confirmed, &QCheckBox::toggled, apply, &QPushButton::setEnabled);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || !confirmed->isChecked())
        return;
    Json request = {{machine ? "machine_id" : "worker_id", target.toStdString()},
        {"confirm_stopped", true},
        {"tasks", expected}};
    managementPending_ = true;
    showWorkers();
    rpc_.request("recover_offline_workers", request, [this](RpcReply r) {
        managementPending_ = false;
        if (!r.ok) {
            showWorkers();
            note(QStringLiteral("故障恢复未确认：") + r.error +
                     QStringLiteral("。请刷新核对；不要重复确认正在运行的新执行。"),
                true);
            refresh();
            return;
        }
        const auto result = r.result;
        int queued = 0, cancelled = 0, failed = 0;
        for (const auto& item : result.value("items", Json::array())) {
            const auto action = text(item, "action");
            queued += action == "requeued";
            cancelled += action == "cancelled";
            failed += action == "failed";
        }
        const bool complete = result.value("complete", false);
        note(
            QStringLiteral(
                "%1重新排队 %2，取消 %3，重试用尽转失败 %4。故障 slot 保持下线；旧执行记录已保留。")
                    .arg(complete ? QString() : QStringLiteral("部分完成，请刷新核对！ "))
                    .arg(queued)
                    .arg(cancelled)
                    .arg(failed) +
                (complete ? QString() : " " + text(result, "error")),
            !complete);
        refresh();
    });
}

void MainWindow::controlWorkers(bool machine, const QString& target, const QString& action) {
    if (target.isEmpty() || managementPending_)
        return;
    if (!demo_ && (workersStale_ || !operations_.contains("control_workers"))) {
        note(QStringLiteral("状态过期或 Server 不支持管理接口，请先刷新 / 升级。"), true);
        return;
    }
    QSignalBlocker blockRefresh(refreshTimer_), blockFreshness(freshnessTimer_);
    QString scope;
    if (action == "update") {
        bool ok = false;
        const auto choice = QInputDialog::getItem(this,
            QStringLiteral("SVN 更新范围"),
            QStringLiteral("仅更新工作机本地的独立 SVN 副本"),
            {QStringLiteral("galaxcore/test2"), QStringLiteral("完整 GalaxCore 副本")},
            0,
            false,
            &ok);
        if (!ok)
            return;
        scope = choice == QStringLiteral("galaxcore/test2") ? "test2" : "all";
    }
    const auto explanation =
        action == "pause"
            ? QStringLiteral(
                  "下线后不再派发新任务，已分配 / 运行中的 testcase 正常结束；通信连接保留。")
        : action == "resume"
            ? QStringLiteral(
                  "仅将当前已连接、未退出且运行策略允许的 slots 上线；Server 跳过模式或资源保护排除的槽位。失联机器不会被远程启动。维护失败后请先检查工作副本。")
            : QStringLiteral(
                  "先停止派发，等待当前执行和结果上报结束，再更新 SVN。不会 revert 或删除本地修改；冲突会保持停派。成功后恢复更新前的接单意向。");
    if (QMessageBox::question(this,
            QStringLiteral("确认工作机管理操作"),
            target + "\n" + explanation) != QMessageBox::Yes)
        return;
    if (demo_) {
        for (auto& w : workers_)
            if (text(w, machine ? "machine_id" : "worker_id") == target) {
                if (action == "resume" && text(w, "status") != "ONLINE")
                    continue;
                if (action == "update")
                    w["maintenance"] = {{"state", "PENDING"}, {"scope", scope.toStdString()}};
                w["accepting_tasks"] = action == "resume";
                w["accepting_requested"] =
                    action == "update" ? w.value("accepting_requested", true) : action == "resume";
            }
        showWorkers();
        note(QStringLiteral("仅演示：已修改本地示例，未向 Server 或 SVN 发送命令。"));
        return;
    }
    Json request = {{machine ? "machine_id" : "worker_id", target.toStdString()},
        {"action", action.toStdString()},
        {"operation_id", QUuid::createUuid().toString().mid(1, 36).toStdString()}};
    if (action == "update")
        request["scope"] = scope.toStdString();
    managementPending_ = true;
    showWorkers();
    rpc_.request("control_workers", request, [this, action](RpcReply r) {
        managementPending_ = false;
        if (!r.ok) {
            showWorkers();
            note(QStringLiteral("管理请求未确认：") + r.error +
                     QStringLiteral("。请刷新核对，不自动重复操作。"),
                true);
            return;
        }
        note(action == "update"
                 ? QStringLiteral("更新请求已接受；请查看维护列，只有 Worker 确认后才算完成。")
                 : QStringLiteral("上线/下线状态已保存，当前 testcase 不受影响。"));
        refresh();
    });
}
