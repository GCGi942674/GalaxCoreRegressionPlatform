#pragma once
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

// Version selection is explicit; a missing original never falls back to latest.
class RerunSelectionDialog : public QDialog {
public:
    RerunSelectionDialog(int count,
        const QStringList& originals,
        const QStringList& available,
        QWidget* parent = nullptr)
        : QDialog(parent)
        , originals_(originals)
        , available_(available) {
        setObjectName("rerunSelectionDialog");
        setWindowTitle(QStringLiteral("确认复测版本"));
        resize(590, 340);
        auto layout = new QVBoxLayout(this);
        auto summary = new QLabel(QStringLiteral("复测 %1 个失败 / 超时案例\n原版本：%2")
                                      .arg(count)
                                      .arg(originals.join(", ")));
        summary->setTextFormat(Qt::PlainText);
        summary->setWordWrap(true);
        layout->addWidget(summary);
        auto form = new QFormLayout;
        mode_ = new QComboBox;
        mode_->setObjectName("rerunVersionMode");
        mode_->addItem(QStringLiteral("原版本复测（各案例保持原版本）"));
        mode_->addItem(QStringLiteral("指定版本验证（所有案例使用所选版本）"));
        version_ = new QComboBox;
        version_->setObjectName("rerunRevision");
        version_->addItem(QStringLiteral("请选择 ZIP 版本"), "");
        for (const auto& revision : available_)
            version_->addItem("GalaxCore_" + revision + ".zip", revision);
        form->addRow(QStringLiteral("复测方式"), mode_);
        form->addRow(QStringLiteral("验证版本"), version_);
        layout->addLayout(form);
        warning_ = new QLabel;
        warning_->setObjectName("rerunVersionWarning");
        warning_->setWordWrap(true);
        warning_->setTextFormat(Qt::PlainText);
        layout->addWidget(warning_);
        auto help = new QLabel(QStringLiteral(
            "沿用原 testcase、flow_config、超时和优先级；创建独立人工批次，不修改历史、不参与每日回归。\nZIP 可能被外部清理，提交后仍可能因文件消失而失败。"));
        help->setWordWrap(true);
        layout->addWidget(help);
        buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons_->button(QDialogButtonBox::Ok)->setText(QStringLiteral("提交复测"));
        layout->addWidget(buttons_);
        connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(mode_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
            updateChoice();
        });
        connect(version_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
            updateChoice();
        });
        updateChoice();
    }

    QString selectedRevision() const {
        return mode_->currentIndex() == 0 ? QString() : version_->currentData().toString();
    }

private:
    void updateChoice() {
        QStringList missing;
        for (const auto& revision : originals_)
            if (!available_.contains(revision))
                missing << revision;
        const bool specified = mode_->currentIndex() == 1;
        version_->setEnabled(specified);
        const bool valid = specified ? !version_->currentData().toString().isEmpty()
                                     : missing.isEmpty() && !originals_.isEmpty();
        if (!specified && !missing.isEmpty())
            warning_->setText(QStringLiteral(
                "原版本 %1 不在服务端可用 ZIP 列表中（可能已清理）。请选择“指定版本验证”并选择一个版本。")
                                  .arg(missing.join(", ")));
        else if (specified)
            warning_->setText(valid ? QStringLiteral("将使用明确选择的新版本，不覆盖原结果。")
                                    : QStringLiteral("请明确选择版本，不会自动使用最新 ZIP。"));
        else
            warning_->setText(QStringLiteral("原版本仍在服务端可用 ZIP 列表中。"));
        buttons_->button(QDialogButtonBox::Ok)->setEnabled(valid);
    }

    QStringList originals_, available_;
    QComboBox *mode_, *version_;
    QLabel* warning_;
    QDialogButtonBox* buttons_;
};
