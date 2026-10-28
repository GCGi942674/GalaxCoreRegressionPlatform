#include "ConsoleSupport.h"
#include "CaseListImport.h"
#include <QTimeEdit>
#include <QRegularExpression>
#include <QRadioButton>
#include <QScrollBar>
#include <optional>

namespace {
class PlanSwitch final : public QCheckBox {
    QVariantAnimation motion_;
    qreal position_{0};

public:
    explicit PlanSwitch(const QString& text)
        : QCheckBox(text) {
        setCursor(Qt::PointingHandCursor);
        motion_.setDuration(150);
        motion_.setEasingCurve(QEasingCurve::OutCubic);
        connect(&motion_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            position_ = value.toReal();
            update();
        });
        connect(this, &QCheckBox::toggled, this, [this](bool on) {
            motion_.stop();
            motion_.setStartValue(position_);
            motion_.setEndValue(on ? 1.0 : 0.0);
            motion_.start();
        });
    }

    QSize sizeHint() const override {
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
        return QSize(44 + fontMetrics().horizontalAdvance(text()), 32);
#else
        return QSize(44 + fontMetrics().width(text()), 32);
#endif
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal y = (height() - 18) / 2.0;
        p.setPen(Qt::NoPen);
        p.setBrush(!isEnabled()  ? QColor("#d7e0ef")
                   : isChecked() ? QColor("#3169e9")
                                 : QColor("#b8c4d6"));
        p.drawRoundedRect(QRectF(0, y, 32, 18), 9, 9);
        p.setBrush(Qt::white);
        p.drawEllipse(QRectF(2 + 14 * position_, y + 2, 14, 14));
        p.setPen(isEnabled() ? QColor("#5b708a") : QColor("#9aa6b8"));
        p.drawText(QRect(40, 0, width() - 40, height()), Qt::AlignLeft | Qt::AlignVCenter, text());
        if (hasFocus()) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor("#8caef4"), 1, Qt::DashLine));
            p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 5, 5);
        }
    }
};

QLabel* caption(const QString& text, const char* name = "muted") {
    auto w = new QLabel(text);
    w->setObjectName(name);
    w->setTextFormat(Qt::PlainText);
    w->setWordWrap(true);
    w->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    return w;
}

QPushButton* action(const QString& text, const char* name) {
    auto w = new QPushButton(text);
    w->setObjectName(name);
    return w;
}

QFrame* panel() {
    auto w = new QFrame;
    w->setObjectName("contentCard");
    return w;
}

QString weekText(const Json& days) {
    const QStringList names{QStringLiteral("一"),
        QStringLiteral("二"),
        QStringLiteral("三"),
        QStringLiteral("四"),
        QStringLiteral("五"),
        QStringLiteral("六"),
        QStringLiteral("日")};
    QStringList out;
    for (const auto& day : days)
        if (day.is_number_integer() && day >= 1 && day <= 7)
            out << names[day.get<int>() - 1];
    return QStringLiteral("周 ") + out.join(" / ");
}

Json receiptFor(const Json& state, const Json& plan) {
    Json latest = Json::object();
    if (!state.is_object())
        return latest;
    for (const auto& r : state.value("runs", Json::array()))
        if (r.value("plan_id", std::string()) == plan.value("id", std::string()) &&
            r.value("date", std::string()) >= latest.value("date", std::string()))
            latest = r;
    return latest;
}

QString receiptText(const Json& run) {
    const auto state = run.value("state", std::string());
    if (state == "SUBMITTED")
        return QStringLiteral("已入队 %1/%1").arg(run.value("planned", 0));
    if (state == "SUBMITTING")
        return QStringLiteral("提交中 %1/%2")
            .arg(run.value("next_index", 0))
            .arg(run.value("planned", 0));
    if (state == "FAILED")
        return QStringLiteral("触发失败");
    if (state == "STOPPED")
        return QStringLiteral("批次已取消");
    return QStringLiteral("尚未触发");
}

QString selectDirectory(QWidget* parent, const Json& catalog, const QString& current) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("选择 Server testcase 目录"));
    dialog.resize(520, 540);
    auto box = new QVBoxLayout(&dialog);
    box->addWidget(
        caption(QStringLiteral("目录来自 Server 的 test2，不读取 GUI 所在机器的文件。")));
    auto search = new QLineEdit;
    search->setPlaceholderText(QStringLiteral("搜索目录"));
    box->addWidget(search);
    auto tree = new QTreeWidget;
    tree->setObjectName("planDirectoryTree");
    tree->setHeaderHidden(true);
    box->addWidget(tree, 1);
    auto all = new QTreeWidgetItem(tree, {QStringLiteral("全部 testcase（.）")});
    all->setData(0, Qt::UserRole, ".");
    QMap<QString, QTreeWidgetItem*> paths;
    paths["."] = all;
    for (const auto& c : catalog.value("cases", Json::array()))
        if (c.is_string()) {
            const auto parts = QString::fromStdString(c.get<std::string>()).split('/');
            QString path;
            auto node = all;
            for (int i = 0; i + 1 < parts.size(); ++i) {
                path += (path.isEmpty() ? "" : "/") + parts[i];
                if (!paths.contains(path)) {
                    auto child = new QTreeWidgetItem(node, {parts[i]});
                    child->setData(0, Qt::UserRole, path);
                    paths[path] = child;
                }
                node = paths[path];
            }
        }
    tree->setCurrentItem(paths.value(current, all));
    all->setExpanded(true);
    QObject::connect(search, &QLineEdit::textChanged, &dialog, [paths](const QString& query) {
        QSet<QString> visible;
        visible.insert(".");
        for (auto it = paths.begin(); it != paths.end(); ++it)
            if (query.isEmpty() || it.key().contains(query, Qt::CaseInsensitive)) {
                auto path = it.key();
                visible.insert(path);
                while (path.contains('/')) {
                    path = path.left(path.lastIndexOf('/'));
                    visible.insert(path);
                }
            }
        for (auto it = paths.begin(); it != paths.end(); ++it) {
            it.value()->setHidden(!visible.contains(it.key()));
            if (!query.isEmpty() && visible.contains(it.key()))
                it.value()->setExpanded(true);
        }
    });
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    box->addWidget(buttons);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("选择目录"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    return dialog.exec() == QDialog::Accepted && tree->currentItem()
               ? tree->currentItem()->data(0, Qt::UserRole).toString()
               : current;
}

std::optional<Json> editPlan(QWidget* parent,
    const Json& original,
    const Json& catalog,
    const QString& timezone,
    bool adding,
    bool supportsList) {
    QDialog dialog(parent, Qt::Dialog | Qt::FramelessWindowHint);
    dialog.setObjectName("automationEditor");
    dialog.setWindowTitle(adding ? QStringLiteral("新建计划") : QStringLiteral("编辑计划"));
    auto host = parent->window();
    QWidget shade(host);
    shade.setObjectName("planEditorShade");
    shade.setStyleSheet("QWidget#planEditorShade{background:rgba(15,30,50,65);}");
    shade.setGeometry(host->rect());
    shade.show();
    shade.raise();
    const int width = qMin(620, host->width() - 30);
    dialog.resize(width, host->height());
    dialog.move(host->mapToGlobal(QPoint(host->width() - width, 0)));
    dialog.setStyleSheet(
        "QDialog#automationEditor{background:white;border-left:1px solid #dce4ee;} QTimeEdit{background:white;border:1px solid #dce4ee;border-radius:8px;padding:8px;} QCheckBox[weekday=\"true\"]{padding:7px;background:#f5f7fb;border-radius:6px;} QCheckBox[weekday=\"true\"]:checked{background:#edf3ff;color:#3169e9;}");
    auto box = new QVBoxLayout(&dialog);
    box->setContentsMargins(24, 24, 24, 18);
    box->setSpacing(14);
    auto headerPanel = new QWidget;
    headerPanel->setObjectName("planEditorHeader");
    auto head = new QHBoxLayout(headerPanel);
    head->setContentsMargins(0, 0, 0, 0);
    auto titles = new QVBoxLayout;
    titles->addWidget(caption("WEEKLY TASK PLAN"));
    titles->addWidget(caption(dialog.windowTitle(), "dialogTitle"));
    titles->addWidget(caption(QStringLiteral("独立验证任务 · 不计入每日回归")));
    head->addLayout(titles, 1);
    auto close = action(QStringLiteral("关闭"), "closePlanEditor");
    head->addWidget(close, 0, Qt::AlignTop);
    box->addWidget(headerPanel);
    QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    auto scroll = new QScrollArea;
    scroll->setObjectName("planEditorScroll");
    scroll->setMinimumHeight(0);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea{background:white;}");
    box->addWidget(scroll, 1);
    auto body = new QWidget;
    body->setObjectName("planEditorBody");
    body->setStyleSheet("QWidget#planEditorBody{background:white;}");
    auto content = new QVBoxLayout(body);
    content->setContentsMargins(0, 8, 8, 8);
    content->setSpacing(14);
    scroll->setWidget(body);
    auto section = [&](const QString& title) {
        auto frame = panel();
        frame->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
        auto layout = new QVBoxLayout(frame);
        layout->setSizeConstraint(QLayout::SetMinimumSize);
        layout->setContentsMargins(18, 18, 18, 18);
        layout->setSpacing(10);
        layout->addWidget(caption(title, "sectionTitle"));
        content->addWidget(frame);
        return layout;
    };
    auto first = section(QStringLiteral("1　执行内容"));
    auto name = new QLineEdit(text(original, "name"));
    name->setObjectName("planName");
    first->addWidget(caption(QStringLiteral("计划名称 *"), "fieldLabel"));
    first->addWidget(name);
    auto stage = new QComboBox;
    stage->setObjectName("planTemplate");
    if (catalog.contains("templates") && catalog["templates"].is_object())
        for (auto it = catalog["templates"].begin(); it != catalog["templates"].end(); ++it)
            stage->addItem(QString::fromStdString(it.key()));
    const auto oldStage = text(original, "template");
    if (!oldStage.isEmpty() && oldStage != "—") {
        if (stage->findText(oldStage) < 0)
            stage->addItem(oldStage);
        stage->setCurrentText(oldStage);
    }
    first->addWidget(caption(QStringLiteral("执行阶段 / Server 模板 *"), "fieldLabel"));
    first->addWidget(stage);
    auto stagesPreview = caption("", "planEnabledStages");
    stagesPreview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    stagesPreview->setStyleSheet(
        "color:#526c91;background:#f3f7fe;border-radius:6px;padding:10px;");
    first->addWidget(stagesPreview);
    auto updateStages = [=] {
        const auto key = stage->currentText().toStdString();
        if (!catalog.contains("templates") || !catalog["templates"].is_object() ||
            !catalog["templates"].contains(key) || !catalog["templates"][key].is_object() ||
            !catalog["templates"][key].contains("flow_config") ||
            !catalog["templates"][key]["flow_config"].is_object()) {
            stagesPreview->setText(
                QStringLiteral("模板配置未加载或模板已移除，无法预览开启阶段。"));
            stagesPreview->setToolTip(QString());
            return;
        }
        const auto& config = catalog["templates"][key]["flow_config"];
        QStringList enabled;
        for (auto it = config.begin(); it != config.end(); ++it)
            if (it.value().is_number_integer() && it.value() != 0)
                enabled << QString::fromStdString(it.key()) + QStringLiteral(" = ") +
                               QString::fromStdString(it.value().dump());
        stagesPreview->setText(enabled.isEmpty()
                                   ? QStringLiteral("当前模板未开启任何阶段（只读）。")
                                   : QStringLiteral("模板已开启 %1 项（只读）：\n%2")
                                         .arg(enabled.size())
                                         .arg(enabled.join(QStringLiteral("　·　"))));
        stagesPreview->setToolTip(QStringLiteral("当前 Server 模板完整 flow_config：\n") +
                                  QString::fromStdString(config.dump(2)));
    };
    QObject::connect(
        stage, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [updateStages](int) {
            updateStages();
        });
    updateStages();
    first->addWidget(caption(QStringLiteral(
        "仅预览当前模板，不在此修改。每轮触发时按当时的 Server 模板固定本轮执行配置。")));
    auto enabled = new QCheckBox(QStringLiteral("保存后启用计划"));
    enabled->setObjectName("planEnabled");
    enabled->setChecked(original.value("enabled", true));
    first->addWidget(enabled);
    auto selectionMode = new QComboBox;
    selectionMode->setObjectName("planSelectionMode");
    selectionMode->addItem(QStringLiteral("测试案例列表 · 每行一个 run.tcl"), "list");
    selectionMode->addItem(QStringLiteral("目录范围 · 递归发现案例"), "directory");
    first->addWidget(caption(QStringLiteral("选择 testcase 的方式 *"), "fieldLabel"));
    first->addWidget(selectionMode);
    auto directoryPanel = new QWidget;
    auto directoryBox = new QVBoxLayout(directoryPanel);
    directoryBox->setContentsMargins(0, 0, 0, 0);
    auto scope = new QLineEdit(text(original, "scope"));
    scope->setObjectName("planScope");
    auto sr = new QHBoxLayout;
    sr->addWidget(scope, 1);
    auto pick = action(QStringLiteral("选择目录"), "pickPlanDirectory");
    sr->addWidget(pick);
    directoryBox->addWidget(caption(QStringLiteral("testcase 目录范围 *"), "fieldLabel"));
    directoryBox->addLayout(sr);
    directoryBox->addWidget(
        caption(QStringLiteral("相对于 Server 的 test2，递归寻找 run.tcl；“.” 表示全部。")));
    first->addWidget(directoryPanel);
    QObject::connect(pick, &QPushButton::clicked, &dialog, [&] {
        scope->setText(selectDirectory(&dialog, catalog, scope->text()));
    });
    auto listPanel = new QWidget;
    auto listBox = new QVBoxLayout(listPanel);
    listBox->setContentsMargins(0, 0, 0, 0);
    auto input = new QPlainTextEdit;
    input->setObjectName("planCaseList");
    input->setMinimumHeight(150);
    input->setPlaceholderText(QStringLiteral(
        "每行一个相对于 test2 的 run.tcl 路径\n可直接粘贴，或导入任意扩展名的 UTF-8 清单"));
    if (original.contains("cases")) {
        QStringList lines;
        for (const auto& path : original["cases"])
            lines << QString::fromStdString(path.get<std::string>());
        input->setPlainText(lines.join('\n'));
    }
    auto importRow = new QHBoxLayout;
    auto import = action(QStringLiteral("导入测试案例列表…"), "importPlanCaseList");
    auto validate = action(QStringLiteral("校验列表"), "validatePlanCaseList");
    importRow->addWidget(import);
    importRow->addWidget(validate);
    importRow->addStretch();
    listBox->addLayout(importRow);
    listBox->addWidget(input);
    auto listStatus =
        caption(QStringLiteral("输入或导入具体案例，不会自动加入整个目录。重复项只执行一次。"));
    listStatus->setObjectName("planCaseListStatus");
    listBox->addWidget(listStatus);
    first->addWidget(listPanel);
    QSet<QString> available;
    for (const auto& c : catalog.value("cases", Json::array()))
        if (c.is_string())
            available.insert(QString::fromStdString(c.get<std::string>()));
    auto parse = [=](const QByteArray& bytes) {
        const auto result = parseCaseList(bytes, available);
        if (result.size() > 20000)
            throw std::runtime_error("一个计划最多选择 20000 个案例");
        return result;
    };
    QObject::connect(validate, &QPushButton::clicked, &dialog, [=] {
        try {
            const auto result = parse(input->toPlainText().toUtf8());
            listStatus->setText(QStringLiteral("校验通过：%1 个唯一 testcase；保存前还会再次校验。")
                                    .arg(result.size()));
        } catch (const std::exception& e) {
            listStatus->setText(QStringLiteral("校验失败：") + QString::fromUtf8(e.what()));
        }
    });
    QObject::connect(input, &QPlainTextEdit::textChanged, &dialog, [=] {
        listStatus->setText(QStringLiteral("列表已修改，加入草稿时将逐行校验。"));
    });
    QObject::connect(import, &QPushButton::clicked, &dialog, [=, &dialog] {
        const auto path = QFileDialog::getOpenFileName(
            &dialog, QStringLiteral("导入测试案例列表"), {}, QStringLiteral("所有文件 (*)"));
        if (path.isEmpty())
            return;
        try {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                throw std::runtime_error("无法打开清单文件");
            const auto bytes = file.read(8 * 1024 * 1024 + 1);
            if (file.error() != QFileDevice::NoError)
                throw std::runtime_error("读取清单失败");
            const auto result = parse(bytes);
            input->setPlainText(result.join('\n'));
            listStatus->setText(QStringLiteral("已导入 %1 个唯一 testcase，替换原列表；尚未保存。")
                                    .arg(result.size()));
        } catch (const std::exception& e) {
            QMessageBox::warning(&dialog,
                QStringLiteral("导入失败"),
                QString::fromUtf8(e.what()) + QStringLiteral("\n原列表保持不变。"));
        }
    });
    const bool listSelected = supportsList && (adding || original.contains("cases"));
    selectionMode->setCurrentIndex(listSelected ? 0 : 1);
    directoryPanel->setVisible(!listSelected);
    listPanel->setVisible(listSelected);
    if (!supportsList) {
        selectionMode->setEnabled(false);
        selectionMode->setToolTip(
            QStringLiteral("此 Server 尚不支持计划案例列表，请升级 Server 后使用。"));
    }
    QObject::connect(selectionMode,
        QOverload<int>::of(&QComboBox::currentIndexChanged),
        &dialog,
        [=](int index) {
            listPanel->setVisible(index == 0);
            directoryPanel->setVisible(index != 0);
        });
    auto second = section(QStringLiteral("2　每周触发时间"));
    second->addWidget(caption(QStringLiteral("执行星期 *"), "fieldLabel"));
    auto days = new QHBoxLayout;
    QList<QCheckBox*> checks;
    const QStringList labels{QStringLiteral("一"),
        QStringLiteral("二"),
        QStringLiteral("三"),
        QStringLiteral("四"),
        QStringLiteral("五"),
        QStringLiteral("六"),
        QStringLiteral("日")};
    for (int i = 1; i <= 7; ++i) {
        auto check = new QCheckBox(labels[i - 1]);
        check->setObjectName("planDay" + QString::number(i));
        check->setProperty("weekday", true);
        check->setChecked(
            std::find(original["weekdays"].begin(), original["weekdays"].end(), Json(i)) !=
            original["weekdays"].end());
        checks << check;
        days->addWidget(check);
    }
    second->addLayout(days);
    auto time = new QTimeEdit(QTime::fromString(text(original, "time"), "HH:mm"));
    time->setObjectName("planTime");
    time->setDisplayFormat("HH:mm");
    second->addWidget(caption(QStringLiteral("触发时间 *"), "fieldLabel"));
    second->addWidget(time);
    second->addWidget(
        caption(QStringLiteral("Server 时区：%1；每天至多一次。错过超过 5 分钟不自动补跑。")
                    .arg(timezone)));
    auto third = section(QStringLiteral("3　版本与运行参数"));
    auto version = new QComboBox;
    version->setObjectName("planRevision");
    version->setEditable(true);
    version->addItem("latest");
    for (const auto& r : catalog.value("revisions", Json::array()))
        if (r.is_string())
            version->addItem(QString::fromStdString(r.get<std::string>()));
    version->setCurrentText(text(original, "revision"));
    auto versionModes = new QHBoxLayout;
    auto latest = new QRadioButton(QStringLiteral("最新可用 ZIP"));
    latest->setObjectName("planLatest");
    auto fixed = new QRadioButton(QStringLiteral("固定版本"));
    fixed->setObjectName("planFixed");
    versionModes->addWidget(latest);
    versionModes->addWidget(fixed);
    versionModes->addStretch();
    third->addLayout(versionModes);
    third->addWidget(version);
    latest->setChecked(version->currentText() == "latest");
    fixed->setChecked(!latest->isChecked());
    version->setVisible(fixed->isChecked());
    QObject::connect(latest, &QRadioButton::toggled, &dialog, [=](bool on) {
        if (on) {
            version->setCurrentText("latest");
            version->hide();
        }
    });
    QObject::connect(fixed, &QRadioButton::toggled, &dialog, [=](bool on) {
        if (on) {
            if (version->currentText() == "latest" && version->count() > 1)
                version->setCurrentIndex(1);
            version->show();
        }
    });
    QObject::connect(version, &QComboBox::currentTextChanged, &dialog, [=](const QString& value) {
        if (value != "latest")
            fixed->setChecked(true);
    });
    third->addWidget(
        caption(QStringLiteral("触发时固定本轮 ZIP 与模板快照，不在提交中途换版本。")));
    auto advanced = action(QStringLiteral("高级设置 · 优先级与单例超时"), "planAdvanced");
    advanced->setCheckable(true);
    third->addWidget(advanced);
    auto details = new QWidget;
    auto form = new QFormLayout(details);
    form->setContentsMargins(0, 4, 0, 0);
    auto priority = new QSpinBox;
    priority->setObjectName("planPriority");
    priority->setRange(-100, 100);
    priority->setValue(original.value("priority", 0));
    form->addRow(QStringLiteral("优先级"), priority);
    auto timeout = new QSpinBox;
    timeout->setObjectName("planTimeout");
    timeout->setRange(1, 86400);
    timeout->setValue(original.value("timeout_seconds", 5400));
    form->addRow(QStringLiteral("固定 / 回退超时（秒）"), timeout);
    auto autoTimeout = new QCheckBox(QStringLiteral("允许自动估算超时"));
    autoTimeout->setObjectName("planAutoTimeout");
    autoTimeout->setEnabled(parent->window()->property("adaptiveTimeoutV1").toBool());
    autoTimeout->setChecked(original.value("timeout_mode", std::string("fixed")) == "auto");
    autoTimeout->setToolTip(QStringLiteral("观察模式或样本不足时，仍使用固定超时。"));
    form->addRow(autoTimeout);
    third->addWidget(details);
    details->hide();
    QObject::connect(advanced, &QPushButton::toggled, details, &QWidget::setVisible);
    content->addStretch();
    auto footerPanel = new QWidget;
    footerPanel->setObjectName("planEditorFooter");
    auto footerBox = new QVBoxLayout(footerPanel);
    footerBox->setContentsMargins(0, 8, 0, 0);
    auto error = caption("", "planError");
    error->setStyleSheet("color:#ad3442");
    footerBox->addWidget(error);
    auto footer = new QHBoxLayout;
    footer->addWidget(caption(QStringLiteral("加入草稿后，需保存更改才生效。")), 1);
    QObject::connect(input, &QPlainTextEdit::textChanged, error, &QLabel::clear);
    QObject::connect(scope, &QLineEdit::textChanged, error, &QLabel::clear);
    QObject::connect(name, &QLineEdit::textChanged, error, &QLabel::clear);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("加入草稿"));
    buttons->button(QDialogButtonBox::Save)->setProperty("kind", "primary");
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    footer->addWidget(buttons);
    footerBox->addLayout(footer);
    box->addWidget(footerPanel);
    std::optional<Json> result;
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        Json selected = Json::array();
        for (int i = 0; i < checks.size(); ++i)
            if (checks[i]->isChecked())
                selected.push_back(i + 1);
        const auto path = scope->text().trimmed(), title = name->text().trimmed(),
                   rev = version->currentText().trimmed();
        if (title.isEmpty() || title.toUtf8().size() > 128 || selected.empty() ||
            stage->currentText().isEmpty() || !time->time().isValid()) {
            error->setText(
                QStringLiteral("名称须为 1–128 字节；请选择星期、Server 模板及有效时间。"));
            return;
        }
        QStringList selectedCases;
        const bool caseList = selectionMode->currentData().toString() == "list";
        if (caseList) {
            try {
                selectedCases = parse(input->toPlainText().toUtf8());
            } catch (const std::exception& e) {
                error->setText(QString::fromUtf8(e.what()));
                return;
            }
        } else {
            const auto parts = path.split('/');
            if (!QRegularExpression("^[A-Za-z0-9_.+/-]+$").match(path).hasMatch() ||
                path.startsWith('/') || path.endsWith('/') || path.contains("//") ||
                parts.contains("..") || (path != "." && parts.contains("."))) {
                error->setText(
                    QStringLiteral("目录必须是 test2 内的相对路径，不能使用绝对路径或 ..。"));
                return;
            }
        }
        if (!QRegularExpression("^(latest|[0-9]{1,32})$").match(rev).hasMatch()) {
            error->setText(QStringLiteral("版本请填写 latest 或数字 ZIP 版本。"));
            return;
        }
        auto p = original;
        p["name"] = title.toStdString();
        p["enabled"] = enabled->isChecked();
        p["weekdays"] = selected;
        p["time"] = time->time().toString("HH:mm").toStdString();
        p["template"] = stage->currentText().toStdString();
        p["scope"] = caseList ? std::string() : path.toStdString();
        p.erase("cases");
        if (caseList) {
            p["cases"] = Json::array();
            for (const auto& c : selectedCases)
                p["cases"].push_back(c.toStdString());
        }
        p["revision"] = rev.toStdString();
        p["priority"] = priority->value();
        p["timeout_seconds"] = timeout->value();
        if (autoTimeout->isChecked())
            p["timeout_mode"] = "auto";
        else
            p.erase("timeout_mode");
        result = p;
        dialog.accept();
    });
    QPropertyAnimation animation(&dialog, "pos");
    animation.setDuration(180);
    const auto end = dialog.pos();
    animation.setStartValue(end + QPoint(70, 0));
    animation.setEndValue(end);
    animation.setEasingCurve(QEasingCurve::OutCubic);
    animation.start();
    dialog.exec();
    return result;
}

class PlanCenter final : public QWidget {
    RpcClient& rpc_;
    std::function<Json()> catalog_;
    Json saved_, rows_ = Json::array();
    bool querying_ = false, replacing_ = false, saving_ = false, conflict_ = false,
         uncertain_ = false, editorOpen_ = false, queryFailed_ = false;
    uint64_t epoch_ = 0;
    QCheckBox* daily_;
    QLabel *status_, *subtitle_, *dailyState_, *count_;
    QTableWidget* list_;
    QLineEdit* search_;
    QComboBox* filter_;
    QPushButton *save_, *reload_, *discard_, *add_;
    QTimer poll_;

    bool dirty() const {
        return saved_.is_object() &&
               (rows_ != saved_.value("plans", Json::array()) ||
                   daily_->isChecked() != saved_.value("daily_enabled", true));
    }

    bool submitting(const Json& p) const {
        if (!saved_.is_object())
            return false;
        for (const auto& r : saved_.value("runs", Json::array()))
            if (r.value("plan_id", std::string()) == p.value("id", std::string()) &&
                r.value("state", std::string()) == "SUBMITTING")
                return true;
        return false;
    }

    bool editable() const {
        return saved_.is_object() && !replacing_ && !saving_ && !conflict_ && !uncertain_;
    }

    void status(const QString& value, bool error = false) {
        status_->setText(value);
        status_->setStyleSheet(error ? "color:#ad3442" : "color:#71849c");
    }

    int index(const std::string& id) const {
        for (size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].value("id", std::string()) == id)
                return int(i);
        return -1;
    }

    void changed() {
        save_->setEnabled(editable() && dirty());
        discard_->setEnabled(dirty() && !saving_);
        add_->setEnabled(editable() && rows_.size() < 32);
        daily_->setEnabled(editable());
        reload_->setEnabled(!querying_ && !saving_);
        subtitle_->setText(saved_.is_object() ? QStringLiteral("%1 · 设置版本 %2 · Server 时区 %3")
                                                    .arg(dirty() ? QStringLiteral("● 有未保存更改")
                                                                 : QStringLiteral("设置已保存"),
                                                        text(saved_, "revision"),
                                                        text(saved_, "server_timezone"))
                                              : QStringLiteral("正在读取 Server 设置…"));
        dailyState_->setText(
            daily_->isChecked() ? QStringLiteral("已开启") : QStringLiteral("已关闭"));
    }

    void adopt(const Json& value) {
        saved_ = value;
        rows_ = value.value("plans", Json::array());
        {
            QSignalBlocker b(daily_);
            daily_->setChecked(value.value("daily_enabled", true));
        }
        conflict_ = false;
        uncertain_ = false;
        paint();
        changed();
    }

    void edit(std::string id) {
        const int i = index(id);
        if (i < 0 || !editable() || submitting(rows_[i]))
            return;
        editorOpen_ = true;
        auto result = editPlan(this,
            rows_[i],
            catalog_(),
            text(saved_, "server_timezone"),
            false,
            saved_.value("case_list_supported", false));
        editorOpen_ = false;
        // Resolve by identity again: receipts can refresh while the modal is open.
        const int current = index(id);
        if (result && current >= 0 && editable() && !submitting(rows_[current])) {
            rows_[current] = *result;
            paint();
            changed();
        }
    }

    void paint() {
        const int scrollY = list_->verticalScrollBar()->value(),
                  scrollX = list_->horizontalScrollBar()->value();
        const auto selected =
            list_->currentRow() >= 0 && list_->item(list_->currentRow(), 0)
                ? list_->item(list_->currentRow(), 0)->data(Qt::UserRole).toString()
                : QString();
        // Retire old cell widgets after their signal stack returns.
        for (int r = 0; r < list_->rowCount(); ++r)
            for (int c : {3, 5})
                if (auto w = list_->cellWidget(r, c)) {
                    list_->removeCellWidget(r, c);
                    w->deleteLater();
                }
        list_->setRowCount(0);
        int enabledCount = 0;
        for (const auto& p : rows_) {
            if (p.value("enabled", false))
                ++enabledCount;
            const auto run = receiptFor(saved_, p);
            const auto state = run.value("state", std::string());
            const auto q = search_->text().trimmed();
            if (!q.isEmpty() &&
                !(text(p, "name") + " " + text(p, "template") + " " + text(p, "scope") + " " +
                    QString::fromStdString(p.value("cases", Json::array()).dump()))
                     .contains(q, Qt::CaseInsensitive))
                continue;
            const auto f = filter_->currentData().toString();
            if ((f == "enabled" && !p.value("enabled", false)) ||
                (f == "paused" && p.value("enabled", false)) ||
                (f == "failed" && state != "FAILED"))
                continue;
            const auto selection =
                p.contains("cases")
                    ? QStringLiteral("案例列表 · %1 个 testcase").arg(int(p["cases"].size()))
                    : QStringLiteral("目录 · ") + text(p, "scope");
            const int row = list_->rowCount();
            list_->insertRow(row);
            const auto id = p.at("id").get<std::string>();
            const QStringList values{text(p, "name") + "\n" + selection,
                text(p, "time") + "  " + weekText(p["weekdays"]) + "\n" + text(p, "template"),
                text(p, "revision") == "latest" ? QStringLiteral("latest · 自动")
                                                : text(p, "revision"),
                "",
                receiptText(run) + (run.empty() ? QString() : "\n" + text(run, "date")),
                ""};
            for (int c = 0; c < values.size(); ++c) {
                auto item = new QTableWidgetItem(values[c]);
                item->setToolTip(c == 4 ? QStringLiteral("双击查看提交记录；不是执行结果。\n") +
                                              text(run, "error")
                                        : values[c]);
                list_->setItem(row, c, item);
            }
            list_->item(row, 0)->setData(Qt::UserRole, QString::fromStdString(id));
            if (state == "FAILED")
                list_->item(row, 4)->setForeground(QColor("#ad3442"));
            auto holder = new QWidget;
            auto tr = new QHBoxLayout(holder);
            auto toggle = new PlanSwitch(
                p.value("enabled", false) ? QStringLiteral("启用") : QStringLiteral("暂停"));
            toggle->setChecked(p.value("enabled", false));
            toggle->setEnabled(editable());
            tr->addWidget(toggle);
            list_->setCellWidget(row, 3, holder);
            connect(toggle, &QCheckBox::toggled, this, [this, id](bool on) {
                const int i = index(id);
                if (i >= 0 && editable()) {
                    rows_[i]["enabled"] = on;
                    QTimer::singleShot(0, this, [this] {
                        paint();
                        changed();
                    });
                }
            });
            auto actions = new QWidget;
            auto ar = new QHBoxLayout(actions);
            ar->setContentsMargins(2, 4, 2, 4);
            ar->setSpacing(4);
            auto e = action(QStringLiteral("编辑"), "editPlan");
            auto d = action(QStringLiteral("删除"), "deletePlan");
            d->setProperty("kind", "danger");
            e->setEnabled(editable() && !submitting(p));
            d->setEnabled(e->isEnabled());
            if (submitting(p)) {
                e->setToolTip(QStringLiteral("正在提交，请暂停或等待本轮提交完成后编辑。"));
                d->setToolTip(e->toolTip());
            }
            ar->addWidget(e);
            ar->addWidget(d);
            list_->setCellWidget(row, 5, actions);
            connect(e, &QPushButton::clicked, this, [this, id] {
                edit(id);
            });
            connect(d, &QPushButton::clicked, this, [this, id] {
                const auto target = id;
                if (!editable())
                    return;
                if (QMessageBox::question(this,
                        QStringLiteral("删除计划"),
                        QStringLiteral(
                            "仅删除定时规则，不删除已提交任务与结果。删除将加入草稿，保存后生效。")) !=
                    QMessageBox::Yes)
                    return;
                const int i = index(target);
                if (i >= 0 && editable() && !submitting(rows_[i])) {
                    rows_.erase(rows_.begin() + i);
                    QTimer::singleShot(0, this, [this] {
                        paint();
                        changed();
                    });
                }
            });
            if (selected == QString::fromStdString(id))
                list_->selectRow(row);
        }
        count_->setText(QStringLiteral("%1 条计划 · %2 已启用 · %3 已暂停%4")
                            .arg(int(rows_.size()))
                            .arg(enabledCount)
                            .arg(int(rows_.size()) - enabledCount)
                            .arg(list_->rowCount() ? QString() : QStringLiteral(" · 没有匹配项")));
        list_->doItemsLayout();
        list_->verticalScrollBar()->setValue(scrollY);
        list_->horizontalScrollBar()->setValue(scrollX);
    }

public:
    PlanCenter(RpcClient& rpc, std::function<Json()> catalog, QWidget* parent)
        : QWidget(parent)
        , rpc_(rpc)
        , catalog_(std::move(catalog)) {
        setObjectName("automationPage");
        auto box = new QVBoxLayout(this);
        box->setContentsMargins(28, 24, 28, 20);
        box->setSpacing(16);
        auto header = new QHBoxLayout;
        auto titles = new QVBoxLayout;
        titles->addWidget(caption(QStringLiteral("计划中心"), "pageTitle"));
        titles->addWidget(caption(QStringLiteral("每日回归与独立验证计划，在同一个地方管理。")));
        subtitle_ = caption("");
        titles->addWidget(subtitle_);
        header->addLayout(titles, 1);
        discard_ = action(QStringLiteral("放弃更改"), "discardAutomation");
        save_ = action(QStringLiteral("保存更改"), "saveAutomation");
        save_->setProperty("kind", "primary");
        header->addWidget(discard_);
        header->addWidget(save_);
        box->addLayout(header);
        auto dailyCard = panel();
        dailyCard->setStyleSheet(
            "QFrame#contentCard{background:#f8faff;border:1px solid #dfe8f8;border-radius:12px;}");
        auto dl = new QVBoxLayout(dailyCard);
        dl->setContentsMargins(22, 20, 22, 20);
        auto dr = new QHBoxLayout;
        dr->addWidget(caption(QStringLiteral("每日回归"), "sectionTitle"));
        dailyState_ = caption("");
        dr->addWidget(dailyState_);
        dr->addStretch();
        daily_ = new PlanSwitch(QStringLiteral("允许每日提交"));
        daily_->setObjectName("dailyEnabled");
        dr->addWidget(daily_);
        dl->addLayout(dr);
        dl->addWidget(caption(
            QStringLiteral("沿用现有定时入口。关闭仅阻止新一轮，已接受的提交与运行不受影响。")));
        dl->addWidget(caption(QStringLiteral(
            "任务来源　Tasks.yaml → templates/*.json　　　触发时间　由现有 crontab 管理")));
        box->addWidget(dailyCard);
        box->addWidget(caption(QStringLiteral("白天 / 每周计划"), "sectionTitle"));
        box->addWidget(caption(
            QStringLiteral("由 Server 独立触发；每个计划每天至多一次，结果不纳入每日回归对比。")));
        auto frame = panel();
        auto fb = new QVBoxLayout(frame);
        fb->setContentsMargins(14, 14, 14, 8);
        auto tools = new QHBoxLayout;
        search_ = new QLineEdit;
        search_->setObjectName("automationSearch");
        search_->setClearButtonEnabled(true);
        search_->setPlaceholderText(QStringLiteral("搜索计划、模板、目录或 testcase"));
        tools->addWidget(search_, 1);
        filter_ = new QComboBox;
        filter_->setObjectName("automationFilter");
        filter_->addItem(QStringLiteral("全部状态"), "");
        filter_->addItem(QStringLiteral("已启用"), "enabled");
        filter_->addItem(QStringLiteral("已暂停"), "paused");
        filter_->addItem(QStringLiteral("触发失败"), "failed");
        tools->addWidget(filter_);
        add_ = action(QStringLiteral("＋ 新建计划"), "addPlan");
        tools->addWidget(add_);
        fb->addLayout(tools);
        count_ = caption("");
        fb->addWidget(count_);
        list_ = table({QStringLiteral("计划 / testcase 范围"),
            QStringLiteral("触发时间 / 模板"),
            QStringLiteral("ZIP 版本"),
            QStringLiteral("启用状态"),
            QStringLiteral("最近一次提交"),
            QStringLiteral("操作")});
        list_->setObjectName("automationTable");
        list_->setWordWrap(true);
        list_->verticalHeader()->setDefaultSectionSize(84);
        list_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        const QList<int> widths{245, 260, 110, 90, 160, 140};
        for (int i = 0; i < widths.size(); ++i)
            list_->setColumnWidth(i, widths[i]);
        list_->horizontalHeader()->setStretchLastSection(true);
        fb->addWidget(list_, 1);
        box->addWidget(frame, 1);
        auto foot = new QHBoxLayout;
        status_ = caption(QStringLiteral("正在连接 Server…"), "automationStatus");
        foot->addWidget(status_, 1);
        reload_ = action(QStringLiteral("重新载入"), "reloadAutomation");
        foot->addWidget(reload_);
        box->addLayout(foot);
        connect(search_, &QLineEdit::textChanged, this, [this] {
            paint();
        });
        connect(filter_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
            paint();
        });
        connect(daily_, &QCheckBox::toggled, this, [this] {
            changed();
        });
        connect(list_, &QTableWidget::cellDoubleClicked, this, [this](int row, int col) {
            if (!list_->item(row, 0))
                return;
            const auto id = list_->item(row, 0)->data(Qt::UserRole).toString().toStdString();
            const int i = index(id);
            if (i < 0)
                return;
            if (col != 4) {
                edit(id);
                return;
            }
            const auto run = receiptFor(saved_, rows_[i]);
            QMessageBox message(this);
            message.setWindowTitle(QStringLiteral("最近提交记录"));
            message.setTextFormat(Qt::PlainText);
            message.setText(
                receiptText(run) +
                QStringLiteral(
                    "\n\n这是提交状态，不代表 testcase 执行完成。执行结果请在任务管理查看。"));
            message.setDetailedText(QString::fromStdString(run.dump(2)));
            message.exec();
        });
        connect(add_, &QPushButton::clicked, this, [this] {
            Json p = {{"id", QUuid::createUuid().toString().mid(1, 36).toStdString()},
                {"name", ""},
                {"enabled", true},
                {"weekdays", {1, 2, 3, 4, 5}},
                {"time", "10:30"},
                {"template", ""},
                {"scope", "."},
                {"revision", "latest"},
                {"priority", 0},
                {"timeout_seconds", 5400}};
            editorOpen_ = true;
            auto result = editPlan(this,
                p,
                catalog_(),
                text(saved_, "server_timezone"),
                true,
                saved_.value("case_list_supported", false));
            editorOpen_ = false;
            if (result && editable() && rows_.size() < 32) {
                rows_.push_back(*result);
                paint();
                changed();
            }
        });
        connect(discard_, &QPushButton::clicked, this, [this] {
            if (QMessageBox::question(this,
                    QStringLiteral("放弃更改"),
                    QStringLiteral("放弃本地未保存的计划修改？")) == QMessageBox::Yes) {
                adopt(saved_);
                load(true);
            }
        });
        connect(reload_, &QPushButton::clicked, this, [this] {
            if (dirty() &&
                QMessageBox::question(this,
                    QStringLiteral("重新载入"),
                    QStringLiteral("重新载入将放弃本地草稿，是否继续？")) != QMessageBox::Yes)
                return;
            load(true);
        });
        connect(save_, &QPushButton::clicked, this, [this] {
            if (!editable() || !dirty())
                return;
            if (rows_.dump().size() > 512 * 1024) {
                status(QStringLiteral("计划配置超过 512 KiB，请缩减清单；草稿已保留，尚未发送。"),
                    true);
                return;
            }
            saving_ = true;
            ++epoch_;
            changed();
            paint();
            status(QStringLiteral("正在保存，请等待 Server 确认…"));
            QPointer<PlanCenter> guard(this);
            rpc_.request("update_automation",
                {{"expected_revision", saved_.at("revision")},
                    {"daily_enabled", daily_->isChecked()},
                    {"plans", rows_}},
                [guard](RpcReply r) {
                    if (!guard)
                        return;
                    guard->saving_ = false;
                    if (!r.ok) {
                        guard->uncertain_ = true;
                        guard->status(QStringLiteral("保存未确认：") + r.error +
                                          QStringLiteral("；请重新载入核对，未自动重试或覆盖。"),
                            true);
                        guard->paint();
                        guard->changed();
                        return;
                    }
                    guard->adopt(r.result);
                    guard->status(QStringLiteral("已保存；不影响正在运行的 testcase。"));
                });
        });
        poll_.setInterval(5000);
        connect(&poll_, &QTimer::timeout, this, [this] {
            if (isVisible())
                load(false);
        });
        poll_.start();
        changed();
        load(true);
    }

    void load(bool replace) {
        if (querying_ || saving_)
            return;
        querying_ = true;
        replacing_ = replace;
        changed();
        if (replace)
            paint();
        const auto request = ++epoch_;
        QPointer<PlanCenter> guard(this);
        rpc_.request("query_automation", Json::object(), [guard, replace, request](RpcReply r) {
            if (!guard)
                return;
            guard->querying_ = false;
            guard->replacing_ = false;
            if (request != guard->epoch_) {
                guard->changed();
                return;
            }
            if (!r.ok) {
                guard->queryFailed_ = true;
                guard->status(QStringLiteral("计划状态未更新：") + r.error +
                                  QStringLiteral("；当前显示仅为上次快照。"),
                    true);
                guard->changed();
                guard->paint();
                return;
            }
            if (guard->queryFailed_ && !guard->conflict_ && !guard->uncertain_)
                guard->status(QStringLiteral("连接已恢复，计划状态已更新。"));
            guard->queryFailed_ = false;
            if (replace || !guard->saved_.is_object()) {
                guard->adopt(r.result);
                guard->status(QStringLiteral("已从 Server 载入；修改后请保存更改。"));
            } else if (r.result.at("revision") != guard->saved_.at("revision")) {
                if (!guard->dirty() && !guard->uncertain_ && !guard->editorOpen_) {
                    guard->adopt(r.result);
                    guard->status(QStringLiteral("已同步其他用户的设置。"));
                } else {
                    guard->conflict_ = true;
                    guard->saved_["runs"] = r.result.value("runs", Json::array());
                    guard->status(
                        QStringLiteral(
                            "其他用户已修改设置。保留本地草稿，请重新载入后再修改，不能覆盖保存。"),
                        true);
                    guard->paint();
                    guard->changed();
                }
            } else {
                guard->saved_["runs"] = r.result.value("runs", Json::array());
                guard->paint();
                guard->changed();
            }
            if (!r.result.value("runner_error", std::string()).empty())
                guard->status(
                    QStringLiteral("计划执行器：") + text(r.result, "runner_error"), true);
        });
    }
};
} // namespace

void MainWindow::openAutomation() {
    if (!operations_.contains("query_automation")) {
        note(QStringLiteral("Server 尚不支持定时计划，请更新 Server 或等待连接完成。"), true);
        return;
    }
    auto center = dynamic_cast<PlanCenter*>(automationPage_);
    if (!center) {
        auto wrapper = automationPage_;
        auto layout = static_cast<QVBoxLayout*>(wrapper->layout());
        while (auto item = layout->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        layout->setContentsMargins(0, 0, 0, 0);
        center = new PlanCenter(
            rpc_,
            [this] {
                return catalog_.is_object() ? catalog_ : Json::object();
            },
            wrapper);
        layout->addWidget(center);
        automationPage_ = center;
    } else
        center->load(false);
    pages_->setCurrentIndex(4);
    breadcrumb_->setText(QStringLiteral("工作空间　 /　 计划中心"));
    const auto nav = findChildren<QPushButton*>("navigation");
    for (int i = 0; i < nav.size(); ++i)
        nav[i]->setChecked(i == 4);
}
