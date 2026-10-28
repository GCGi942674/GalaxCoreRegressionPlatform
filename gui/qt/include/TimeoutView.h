#pragma once
#include <QLabel>
#include <QFrame>
#include <QGridLayout>
#include <QMap>
#include "nlohmann/json.hpp"

namespace GcrpUi {
inline QString timeoutReason(const std::string& reason) {
    static const QMap<QString, QString> names{
        {"insufficient_data", QStringLiteral("尚无可比历史，使用固定超时")},
        {"insufficient_samples", QStringLiteral("历史样本不足，使用固定超时")},
        {"observe_only", QStringLiteral("观察模式：仅计算建议，仍使用固定超时")},
        {"fixed_requested", QStringLiteral("本任务选择固定超时")},
        {"observation_not_qualified", QStringLiteral("观察验证尚未达标")},
        {"worker_not_supported", QStringLiteral("工作机尚不支持自动超时")},
        {"resource_not_verified", QStringLiteral("资源数据不足，使用固定超时")},
        {"resource_pressure", QStringLiteral("机器资源压力较高，使用固定超时")},
        {"estimate_exceeds_cap", QStringLiteral("建议超过固定预算，需人工检查")},
        {"auto_timeout_guard", QStringLiteral("曾触发自动超时，已暂停自动缩短")},
        {"auto_applied", QStringLiteral("已采用自动超时")},
        {"script_fingerprint_changed", QStringLiteral("执行脚本已变化，工作机恢复固定超时")},
        {"archive_fingerprint_changed", QStringLiteral("ZIP 内容已变化，工作机恢复固定超时")},
        {"fixed", QStringLiteral("固定超时")},
        {"observe", QStringLiteral("观察模式")},
        {"disabled", QStringLiteral("自动估算未启用")}};
    return names.value(QString::fromStdString(reason), QString::fromStdString(reason));
}

inline QString timeoutSeconds(const nlohmann::json& value, double divisor = 1) {
    return value.is_number() ? QStringLiteral("%1 秒").arg(
                                   value.get<double>() / divisor, 0, 'f', divisor == 1000 ? 1 : 0)
                             : QStringLiteral("未采集");
}

inline QString compactTimeout(const nlohmann::json& task) {
    const auto e = task.value("execution", nlohmann::json::object());
    const auto timing =
        e.is_object() ? e.value("timing", nlohmann::json::object()) : nlohmann::json::object();
    const auto d = task.value("timeout_decision", nlohmann::json::object());
    const auto p = task.value("payload", nlohmann::json::object());
    const auto actual = timing.value("effective_timeout_seconds",
        d.is_object() ? d.value("effective_seconds", p.value("timeout_seconds", nlohmann::json()))
                      : p.value("timeout_seconds", nlohmann::json()));
    return timeoutSeconds(timing.value("command_wall_ms", nlohmann::json()), 1000) +
           QStringLiteral(" / ") + timeoutSeconds(actual);
}

inline QWidget* timeoutCard(const nlohmann::json& task) {
    auto card = new QFrame;
    card->setObjectName("timeoutCard");
    card->setStyleSheet(
        "#timeoutCard {background:#f3f7fc;border:1px solid #dce6f2;border-radius:10px;} "
        "#timeoutCard QLabel {background:transparent;border:0;}");
    auto layout = new QGridLayout(card);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setHorizontalSpacing(24);
    const auto e = task.value("execution", nlohmann::json::object());
    const auto t =
        e.is_object() ? e.value("timing", nlohmann::json::object()) : nlohmann::json::object();
    const auto d0 = task.value("timeout_decision", nlohmann::json::object());
    const auto d = d0.is_object() ? d0 : nlohmann::json::object();
    const auto p = task.value("payload", nlohmann::json::object());
    const auto budget = t.value("effective_timeout_seconds",
        d.value("effective_seconds", p.value("timeout_seconds", nlohmann::json())));
    const QStringList titles{
        QStringLiteral("实际执行用时"), QStringLiteral("本次超时"), QStringLiteral("建议超时")};
    const QStringList values{timeoutSeconds(t.value("command_wall_ms", nlohmann::json()), 1000),
        timeoutSeconds(budget),
        timeoutSeconds(d.value("recommended_seconds", nlohmann::json()))};
    for (int i = 0; i < 3; ++i) {
        auto title = new QLabel(titles[i]);
        title->setStyleSheet("color:#71839d;");
        auto value = new QLabel(values[i]);
        value->setStyleSheet("font-size:18px;font-weight:600;color:#1b3559;");
        value->setObjectName(QStringLiteral("timeoutValue%1").arg(i));
        layout->addWidget(title, 0, i);
        layout->addWidget(value, 1, i);
    }
    QString reason = timeoutReason(d.value("fallback_reason", std::string("fixed")));
    if (t.value("fallback_reason", std::string()) == "script_fingerprint_changed" ||
        t.value("fallback_reason", std::string()) == "archive_fingerprint_changed")
        reason = timeoutReason(t["fallback_reason"].get<std::string>());
    reason += QStringLiteral(" · 成功样本 %1 / 本机 %2")
                  .arg(d.value("sample_count", 0))
                  .arg(d.value("machine_sample_count", 0));
    auto hint = new QLabel(reason);
    hint->setWordWrap(true);
    hint->setObjectName("timeoutReason");
    hint->setStyleSheet("color:#61758f;");
    layout->addWidget(hint, 2, 0, 1, 3);
    card->setToolTip(QStringLiteral(
        "只计算单例 run.sh 执行耗时；不含排队、ZIP 准备和清理。旧记录未采集时不会显示虚假的 0 秒。"));
    return card;
}
} // namespace GcrpUi
