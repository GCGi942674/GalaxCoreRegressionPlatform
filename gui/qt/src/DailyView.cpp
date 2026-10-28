#include "ConsoleSupport.h"

void MainWindow::showDailyPlan() {
    if (demo_) {
        note(QStringLiteral("演示模式没有真实 Clock 计划；请连接 Server 查看。"));
        return;
    }
    if (!operations_.contains("query_daily_plan")) {
        note(QStringLiteral("Server 尚不支持每日计划，请先升级。"), true);
        return;
    }
    auto dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName("dailyPlanDialog");
    dialog->setWindowTitle(QStringLiteral("每日回归完整性"));
    dialog->resize(1180, 620);
    auto box = new QVBoxLayout(dialog);
    auto message = new QLabel;
    message->setWordWrap(true);
    message->setTextFormat(Qt::PlainText);
    box->addWidget(message);
    auto rows = table({"Tasks.yaml 任务",
        "模板 / 版本",
        "计划 / 已提交 / 未提交",
        "排队或运行 / 已结束 / 取消",
        "日志已校验 / 已过期",
        "状态"});
    box->addWidget(rows, 1);
    auto buttons = new QHBoxLayout;
    auto refresh = new QPushButton(QStringLiteral("刷新 / 首页"));
    auto next = new QPushButton(QStringLiteral("下一页"));
    next->setEnabled(false);
    buttons->addWidget(refresh);
    buttons->addWidget(next);
    box->addLayout(buttons);
    auto offset = std::make_shared<uint64_t>(0);
    const auto day = dateB_->currentData().toString().toStdString();
    if (day.empty()) {
        note(QStringLiteral("请先选择一个有记录的日期。"));
        dialog->deleteLater();
        return;
    }
    QPointer<QDialog> guard(dialog);
    auto load = [this, guard, day, rows, message, refresh, next, offset](bool first) {
        if (first)
            *offset = 0;
        refresh->setEnabled(false);
        next->setEnabled(false);
        rpc_.request("query_daily_plan",
            {{"date", day}, {"offset", *offset}, {"limit", 50}},
            [this, guard, rows, message, refresh, next, offset, day](RpcReply r) {
                if (!guard)
                    return;
                refresh->setEnabled(true);
                if (!r.ok) {
                    message->setText(QStringLiteral("查询失败：") + r.error);
                    return;
                }
                try {
                    rows->setRowCount(0);
                    if (!r.result.at("plan_available").get<bool>()) {
                        message->setText(QStringLiteral(
                            "%1 没有发布计划。可能 Clock 尚未运行，或是旧版本历史；不能据此判断今日全部完成。")
                                             .arg(QString::fromStdString(day)));
                        return;
                    }
                    const QMap<QString, QString> labels{{"NOT_SUBMITTED", QStringLiteral("未提交")},
                        {"PARTIALLY_SUBMITTED", QStringLiteral("部分提交 / 请检查 Clock")},
                        {"RUNNING", QStringLiteral("执行中")},
                        {"COMPLETE", QStringLiteral("执行完成")},
                        {"COUNT_MISMATCH", QStringLiteral("数量异常")},
                        {"STOPPED", QStringLiteral("已停止后续")},
                        {"STOPPING", QStringLiteral("停止中 · 等待在途完成")},
                        {"DELETED", QStringLiteral("任务记录已删除")},
                        {"PARTIALLY_DELETED", QStringLiteral("部分案例记录已删除")}};
                    for (const auto& item : r.result.at("items")) {
                        const int row = rows->rowCount();
                        rows->insertRow(row);
                        cell(rows, row, 0, text(item, "name"));
                        rows->item(row, 0)->setToolTip(text(item, "batch_id"));
                        cell(rows, row, 1, text(item, "template") + " / " + text(item, "revision"));
                        cell(rows,
                            row,
                            2,
                            text(item, "planned") + " / " + text(item, "submitted") + " / " +
                                text(item, "not_submitted"));
                        cell(rows,
                            row,
                            3,
                            text(item, "active") + " / " + text(item, "finished") + " / " +
                                text(item, "cancelled"));
                        cell(rows,
                            row,
                            4,
                            text(item, "logs_verified") + " / " + text(item, "logs_expired"));
                        cell(rows, row, 5, labels.value(text(item, "state"), text(item, "state")));
                    }
                    message->setText(QStringLiteral(
                        "%1 · %2 个计划批次，本页 %3 个。\n“执行完成”要求计划数量与实际提交数量一致且无在途案例；日志校验是另一条独立状态。未提交项保留，即使尚无 testcase 记录。\n日志统计只计入当前 run 的完整文件集校验；旧 Worker 无清单时不推断。分页为实时数据，请刷新确认。")
                                         .arg(QString::fromStdString(day))
                                         .arg(r.result.at("total").get<uint64_t>())
                                         .arg(rows->rowCount()));
                    next->setEnabled(r.result.value("has_more", false));
                    if (next->isEnabled())
                        *offset = r.result.at("next_offset").get<uint64_t>();
                } catch (const std::exception& e) {
                    message->setText(QString::fromUtf8(e.what()));
                }
            });
    };
    connect(refresh, &QPushButton::clicked, dialog, [load] {
        load(true);
    });
    connect(next, &QPushButton::clicked, dialog, [load] {
        load(false);
    });
    dialog->show();
    load(true);
}
