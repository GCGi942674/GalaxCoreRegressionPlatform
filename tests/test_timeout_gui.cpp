#include <QApplication>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QPushButton>
#include <QTableWidget>
#include <QHeaderView>
#include "TimeoutView.h"
#include <iostream>
using Json = nlohmann::json;

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Json task = {{"payload", {{"timeout_seconds", 5400}}},
        {"execution",
            {{"timing",
                {{"command_wall_ms", 240500},
                    {"effective_timeout_seconds", 5400},
                    {"fallback_reason", "observe"}}}}},
        {"timeout_decision",
            {{"recommended_seconds", 540},
                {"effective_seconds", 5400},
                {"mode", "observe"},
                {"sample_count", 12},
                {"machine_sample_count", 6},
                {"fallback_reason", "observe_only"}}}};
    QWidget window;
    window.setWindowTitle(QStringLiteral("自适应超时 · 离线测试预览"));
    window.resize(1050, 440);
    window.setStyleSheet(
        "QWidget {font-family:Sans Serif;color:#233855;background:#ffffff;font-size:13px;} QPushButton {padding:8px 16px;background:#edf3fb;border:1px solid #d5e1f1;border-radius:6px;}");
    auto layout = new QVBoxLayout(&window);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(18);
    auto title = new QLabel(QStringLiteral("Testcase 执行详情"));
    title->setStyleSheet("font-size:23px;font-weight:600;");
    layout->addWidget(title);
    auto name = new QLabel(QStringLiteral("family / part / case+name · place_design"));
    layout->addWidget(name);
    auto card = GcrpUi::timeoutCard(task);
    layout->addWidget(card);
    auto value = card->findChild<QLabel*>("timeoutValue0");
    if (!value || !value->text().contains("240.5"))
        return 1;
    if (!card->findChild<QLabel*>("timeoutReason")->text().contains(QStringLiteral("观察模式")))
        return 2;
    auto old = GcrpUi::timeoutCard(Json{{"payload", {{"timeout_seconds", 5400}}}});
    if (old->findChild<QLabel*>("timeoutValue0")->text() != QStringLiteral("未采集"))
        return 3;
    delete old;
    auto line = new QCheckBox(QStringLiteral("允许自动估算超时 · 观察模式仍执行原固定预算"));
    layout->addWidget(line);
    auto hint =
        new QLabel(QStringLiteral("建议在派发前生成；排队、ZIP 准备和清理不计入实际执行用时。"));
    hint->setStyleSheet("color:#71839b;");
    layout->addWidget(hint);
    layout->addStretch();
    auto close = new QPushButton(QStringLiteral("关闭"));
    layout->addWidget(close, 0, Qt::AlignRight);
    window.show();
    app.processEvents();
    if (argc > 1 && !window.grab().save(QString::fromLocal8Bit(argv[1])))
        return 4;
    std::cout << "PASS: Qt timing card, old-record placeholder, compact text\n";
    return 0;
}
