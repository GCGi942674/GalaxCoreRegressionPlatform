#include "ConsoleSupport.h"
#include "DailyStyle.h"
#include <QCloseEvent>
#include <QRegularExpression>

namespace {
class EvidenceDrawer final : public QDialog {
public:
    explicit EvidenceDrawer(QWidget* owner)
        : QDialog(owner, Qt::Dialog | Qt::FramelessWindowHint)
        , owner_(owner) {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowModality(Qt::WindowModal);
        shade_ = new QWidget(owner);
        shade_->setStyleSheet("background:rgba(3,11,23,175);");
        shade_->setGeometry(owner->rect());
        shade_->show();
        shade_->raise();
        owner_->installEventFilter(this);
        setStyleSheet(dailyStyle());
    }

    ~EvidenceDrawer() override {
        if (owner_)
            owner_->removeEventFilter(this);
        delete shade_;
    }

    void reveal() {
        const auto target = targetGeometry();
        setGeometry(target.translated(target.width(), 0));
        show();
        raise();
        auto animation = new QPropertyAnimation(this, "geometry", this);
        animation->setDuration(200);
        animation->setStartValue(geometry());
        animation->setEndValue(target);
        animation->setEasingCurve(QEasingCurve::OutCubic);
        animation->start(QAbstractAnimation::DeleteWhenStopped);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == owner_ &&
            (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
            if (shade_)
                shade_->setGeometry(owner_->rect());
            setGeometry(targetGeometry());
        }
        return QDialog::eventFilter(watched, event);
    }

    void closeEvent(QCloseEvent* event) override {
        if (shade_)
            shade_->hide();
        QDialog::closeEvent(event);
    }

private:
    QRect targetGeometry() const {
        const auto origin = owner_->mapToGlobal(QPoint(0, 0));
        const int width = qMin(920, qMax(700, owner_->width() * 3 / 4));
        return QRect(origin.x() + owner_->width() - width, origin.y(), width, owner_->height());
    }

    QPointer<QWidget> owner_, shade_;
};

struct EvidencePane {
    Json task;
    Json files = Json::array();
    bool manifestReady{false};
    QString unavailable;
    uint64_t offset{0}, requestEpoch{0};
    QLabel* status{};
    QPlainTextEdit* body{};
    QPushButton* next{};
};
} // namespace

void MainWindow::openDailyEvidence(
    const Json& row, const QString& baseline, const QString& comparison) {
    auto dialog = new EvidenceDrawer(this);
    dialog->setObjectName("dailyEvidenceDrawer");
    dialog->setWindowTitle(QStringLiteral("两天日志比较 · run / flow_config"));
    auto layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(24, 24, 24, 20);
    layout->setSpacing(16);
    auto top = new QHBoxLayout;
    auto title = new QLabel;
    const auto path = text(row, "case");
    auto parts = path.split('/');
    if (parts.size() > 1)
        parts.removeLast();
    title->setText(parts.isEmpty() ? path : parts.last());
    title->setTextFormat(Qt::PlainText);
    title->setObjectName("pageTitle");
    top->addWidget(title, 1);
    auto close = new QPushButton(QStringLiteral("关闭"));
    close->setObjectName("closeDailyEvidence");
    top->addWidget(close);
    connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    layout->addLayout(top);
    auto description = new QLabel(path + QStringLiteral("\n阶段：") + text(row, "template"));
    description->setTextFormat(Qt::PlainText);
    description->setWordWrap(true);
    description->setObjectName("muted");
    layout->addWidget(description);
    auto tabs = new QHBoxLayout;
    auto run = new QPushButton(QStringLiteral("run · 执行输出"));
    auto flow = new QPushButton(QStringLiteral("flow_config · 阶段配置"));
    run->setObjectName("compareRun");
    flow->setObjectName("compareFlowConfig");
    run->setCheckable(true);
    flow->setCheckable(true);
    run->setChecked(true);
    tabs->addWidget(run);
    tabs->addWidget(flow);
    tabs->addStretch();
    layout->addLayout(tabs);
    auto split = new QSplitter(Qt::Horizontal);
    split->setChildrenCollapsible(false);
    layout->addWidget(split, 1);
    auto panes = std::make_shared<std::vector<EvidencePane>>(2);
    auto file = std::make_shared<QString>("run");
    auto hint = new QLabel(QStringLiteral(
        "真实 Server 归档，默认查看 testcase 原始 run，不使用 run.log 替代。文件较大时按片段读取，两侧可独立翻页。"));
    hint->setWordWrap(true);
    hint->setObjectName("muted");
    layout->addWidget(hint);
    for (int side = 0; side < 2; ++side) {
        auto& pane = (*panes)[side];
        pane.task = row.at(side == 0 ? "baseline" : "comparison");
        auto container = new QWidget;
        auto box = new QVBoxLayout(container);
        box->setContentsMargins(0, 0, 0, 0);
        box->setSpacing(10);
        const auto day = side == 0 ? baseline : comparison;
        auto header =
            new QLabel((side == 0 ? QStringLiteral("基准 · ") : QStringLiteral("对比 · ")) + day +
                       (pane.task.is_null() ? QStringLiteral("\n缺少当日结果")
                                            : " · r" + text(pane.task, "revision") + "\n" +
                                                  text(pane.task, "outcome") + " · Task #" +
                                                  text(pane.task, "task_id")));
        header->setTextFormat(Qt::PlainText);
        header->setWordWrap(true);
        header->setObjectName("dailyHeading");
        box->addWidget(header);
        pane.body = new QPlainTextEdit;
        pane.body->setObjectName(side == 0 ? "baselineEvidence" : "comparisonEvidence");
        pane.body->setReadOnly(true);
        box->addWidget(pane.body, 1);
        pane.status = new QLabel(QStringLiteral("读取归档清单…"));
        pane.status->setTextFormat(Qt::PlainText);
        pane.status->setWordWrap(true);
        pane.status->setObjectName("muted");
        box->addWidget(pane.status);
        pane.next = new QPushButton(QStringLiteral("下一段"));
        pane.next->setEnabled(false);
        pane.next->setObjectName(side == 0 ? "baselineEvidenceNext" : "comparisonEvidenceNext");
        box->addWidget(pane.next);
        split->addWidget(container);
    }
    split->setSizes({430, 430});
    QPointer<QDialog> guard(dialog);
    auto read = std::make_shared<std::function<void(int, bool)>>();
    *read = [this, guard, panes, file](int side, bool first) {
        if (!guard)
            return;
        auto& pane = (*panes)[side];
        const auto epoch = ++pane.requestEpoch;
        pane.next->setEnabled(false);
        pane.body->clear();
        if (first)
            pane.offset = 0;
        if (pane.task.is_null()) {
            pane.status->setText(QStringLiteral("这一天没有结果，不能推断为失败。"));
            return;
        }
        const auto status = text(pane.task, "status"), runId = text(pane.task, "run_id");
        if (status == "PENDING" || status == "QUEUED" || status == "ASSIGNED" ||
            status == "RUNNING") {
            pane.status->setText(QStringLiteral("案例尚未结束，当前接口仅提供执行结束后的归档。"));
            return;
        }
        if (runId.isEmpty() || runId == "—") {
            pane.status->setText(QStringLiteral("没有实际执行编号，暂无执行日志。"));
            return;
        }
        if (!pane.manifestReady) {
            pane.status->setText(pane.unavailable.isEmpty() ? QStringLiteral("正在查询归档清单…")
                                                            : pane.unavailable);
            return;
        }
        QString name;
        bool complete = false;
        for (const auto& item : pane.files) {
            const auto candidate = text(item, "name");
            if ((*file == "run" && (candidate == "evidence.run" || candidate == "run")) ||
                (*file == "flow_config" && candidate == "flow_config")) {
                name = candidate;
                complete = item.value("complete", false);
                if (candidate == "evidence.run" || *file == "flow_config")
                    break;
            }
        }
        if (name.isEmpty()) {
            pane.status->setText(
                *file + QStringLiteral(" 未归档 / 尚未上传；不会使用其他日志冒充。"));
            return;
        }
        const auto offset = pane.offset;
        pane.status->setText(QStringLiteral("正在读取 ") + *file + "…");
        rpc_.request("read_log",
            {{"task_id", pane.task.at("task_id")},
                {"run_id", pane.task.at("run_id")},
                {"name", name.toStdString()},
                {"offset", offset},
                {"limit", settings_.logPageBytes}},
            [guard, panes, side, epoch, offset, name, complete](RpcReply reply) {
                if (!guard)
                    return;
                auto& pane = (*panes)[side];
                if (epoch != pane.requestEpoch)
                    return;
                if (!reply.ok) {
                    pane.status->setText(QStringLiteral("读取失败：") + reply.error);
                    return;
                }
                try {
                    const auto hex =
                        QString::fromStdString(reply.result.at("data_hex").get<std::string>());
                    if (hex.size() % 2 ||
                        !QRegularExpression("^[0-9a-fA-F]*$").match(hex).hasMatch())
                        throw std::runtime_error("invalid archive bytes");
                    const auto bytes = QByteArray::fromHex(hex.toLatin1());
                    const auto end = reply.result.at("next_offset").get<uint64_t>(),
                               size = reply.result.at("size").get<uint64_t>();
                    if (end < offset || end > size || end - offset != uint64_t(bytes.size()) ||
                        (end == offset && end < size))
                        throw std::runtime_error("invalid archive offsets");
                    pane.offset = end;
                    pane.body->setPlainText(QString::fromUtf8(bytes));
                    pane.next->setEnabled(end < size);
                    pane.status->setText(QStringLiteral(
                        "%1 · [%2, %3) / %4 字节 · %5\nUTF-8 跨片段字符可能替换显示。")
                                             .arg(name)
                                             .arg(qulonglong(offset))
                                             .arg(qulonglong(end))
                                             .arg(qulonglong(size))
                                             .arg(complete && reply.result.value("complete", false)
                                                      ? QStringLiteral("文件上传完成")
                                                      : QStringLiteral("上传中 / 非完整证据")));
                } catch (const std::exception& e) {
                    pane.status->setText(QString::fromUtf8(e.what()));
                }
            });
    };
    for (int side = 0; side < 2; ++side) {
        connect((*panes)[side].next, &QPushButton::clicked, dialog, [read, side] {
            (*read)(side, false);
        });
    }
    auto choose = [file, read, run, flow](const QString& value) {
        *file = value;
        run->setChecked(value == "run");
        flow->setChecked(value == "flow_config");
        (*read)(0, true);
        (*read)(1, true);
    };
    connect(run, &QPushButton::clicked, dialog, [choose] {
        choose("run");
    });
    connect(flow, &QPushButton::clicked, dialog, [choose] {
        choose("flow_config");
    });
    dialog->reveal();
    for (int side = 0; side < 2; ++side) {
        auto& pane = (*panes)[side];
        (*read)(side, true);
        const auto status = text(pane.task, "status"), runId = text(pane.task, "run_id");
        if (pane.task.is_null() || runId.isEmpty() || runId == "—" || status == "PENDING" ||
            status == "QUEUED" || status == "ASSIGNED" || status == "RUNNING")
            continue;
        rpc_.request("query_logs",
            {{"task_id", pane.task.at("task_id")}, {"run_id", pane.task.at("run_id")}},
            [guard, panes, side, read](RpcReply reply) {
                if (!guard)
                    return;
                auto& pane = (*panes)[side];
                if (!reply.ok) {
                    pane.unavailable = QStringLiteral("归档查询失败：") + reply.error;
                    pane.status->setText(pane.unavailable);
                    return;
                }
                try {
                    if (reply.result.value("archive_state", std::string()) == "EXPIRED") {
                        pane.unavailable =
                            QStringLiteral("归档已按日志保留期清理，任务结果仍保留。");
                        pane.status->setText(pane.unavailable);
                        return;
                    }
                    pane.files = reply.result.at("items");
                    if (!pane.files.is_array())
                        throw std::runtime_error("invalid archive manifest");
                    pane.manifestReady = true;
                    (*read)(side, true);
                } catch (const std::exception& e) {
                    pane.unavailable = QString::fromUtf8(e.what());
                    pane.status->setText(pane.unavailable);
                }
            });
    }
}
