#pragma once
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QPainter>
#include <QIcon>
#include <QVBoxLayout>
#include <QStackedWidget>
#include <QLabel>
#include <QApplication>
#include <QPushButton>
#include <QEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QVariantAnimation>
#include <QDialog>
#include <QTimer>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QResizeEvent>

namespace GcrpUi {
inline QString styleSheet() {
    return QStringLiteral(R"QSS(

QWidget { font-family:"Microsoft YaHei","Noto Sans CJK SC","Segoe UI",sans-serif; font-size:13px; color:#172b48; }
QMainWindow, QWidget#surface, QWidget#workspace { background:#f5f7fb; }
QWidget#sidebar { background:white; border-right:1px solid #e5ebf3; }
QWidget#topbar { background:white; border-bottom:1px solid #e5ebf3; }
QLabel#brand { font-size:22px; font-weight:700; color:#172b48; }
QLabel#brandBadge { background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #4781f5,stop:1 #2455d0); color:white; border-radius:12px; font-size:25px; font-weight:600; }
QLabel#navCaption { color:#9aa6b8; font-size:11px; padding:10px 14px 2px; }
QLabel#navCount { background:#dce8ff; border-radius:5px; color:#3169e9; font-size:10px; }
QLabel#sidebarFooter { color:#8493a8; font-size:11px; padding:18px 5px 0; }
QLabel#pageTitle { font-size:25px; font-weight:600; color:#172b48; padding:0; }
QLabel#dialogTitle { font-size:21px; font-weight:600; }
QLabel#sectionTitle { font-size:16px; font-weight:600; }
QLabel#breadcrumb { color:#65758d; font-size:13px; }
QLabel#connection { color:#6d7d93; font-size:11px; }
QLabel#muted { color:#8493a8; font-size:11px; }
QLabel#fieldLabel { color:#65758d; font-size:12px; padding-top:4px; }
QLabel#demoBanner { color:#7286a1; background:#edf2f9; padding:8px 28px; font-size:11px; }
QLabel#notice { color:#5f7fac; background:#edf3ff; border-radius:7px; padding:13px 15px; font-size:11px; }
QFrame#contentCard, QFrame#metricCard { background:white; border:1px solid #e5ebf3; border-radius:10px; }
QLabel#metricValue { font-size:29px; font-weight:500; color:#172b48; padding-top:4px; }
QPushButton { background:white; border:1px solid #dce4ee; border-radius:8px; padding:8px 12px; min-height:17px; color:#344963; }
QPushButton:hover { background:#f2f6fd; border-color:#bccce5; }
QPushButton:pressed { background:#e5edfc; padding-top:9px; padding-bottom:7px; }
QPushButton:focus { border-color:#8caef4; }
QPushButton[kind="primary"] { background:#3169e9; border-color:#3169e9; color:white; font-weight:600; }
QPushButton[kind="primary"]:hover { background:#2458d1; border-color:#2458d1; }
QPushButton[kind="danger"] { color:#c14450; border-color:#f0d5d8; }
QPushButton[kind="danger"]:hover { background:#fff1f2; }
QPushButton[kind="quiet"] { border-color:transparent; color:#7286a1; }
QPushButton#navigation { text-align:left; background:transparent; border:0; border-radius:9px; padding:12px 10px; margin-top:4px; color:#697a91; }
QPushButton#navigation:hover { background:#f2f6fd; color:#3169e9; }
QPushButton#navigation:checked { background:#edf3ff; color:#3169e9; font-weight:600; }
QPushButton:disabled, QPushButton[kind="primary"]:disabled { background:#f5f7fb; border-color:#e5ebf3; color:#a7b2c2; }
QLineEdit,QComboBox,QSpinBox,QDateEdit { background:white; border:1px solid #dce4ee; border-radius:8px; padding:8px 10px; min-height:18px; selection-background-color:#dce8ff; }
QLineEdit:focus,QComboBox:focus,QSpinBox:focus,QDateEdit:focus { border-color:#8caef4; }
QComboBox::drop-down,QDateEdit::drop-down { border:0; width:23px; }
QComboBox QAbstractItemView { background:white; selection-background-color:#edf3ff; selection-color:#3169e9; padding:6px; border:1px solid #dce4ee; }
QTreeWidget,QTableWidget,QListWidget { background:white; border:0; outline:0; alternate-background-color:white; selection-background-color:#edf3ff; selection-color:#2459c8; }
QTreeWidget::item,QTableWidget::item { border-bottom:1px solid #edf1f7; padding:8px 6px; }
QTreeWidget::item:hover,QTableWidget::item:hover { background:#f8faff; }
QTreeWidget::item:selected,QTableWidget::item:selected { background:#edf3ff; color:#2459c8; }
QHeaderView::section { background:#f8faff; color:#8191a7; font-size:11px; font-weight:500; border:0; border-bottom:1px solid #e5ebf3; padding:12px 8px; }
QListWidget#hostList::item { padding:5px; margin:4px 0; border:1px solid transparent; border-radius:9px; }
QListWidget#hostList::item:selected { border-color:#bdd0f5; background:#edf3ff; }
QListWidget#hostList::item:hover { background:#f5f8fe; }
QListWidget#logFiles { background:#f8faff; padding:10px 6px; }
QListWidget#logFiles::item { padding:11px 8px; margin:2px; border-radius:6px; color:#7286a1; }
QListWidget#logFiles::item:selected { background:#e7effd; color:#3665ac; }
QPlainTextEdit { border:1px solid #e5ebf3; border-radius:8px; background:white; padding:12px; }
QPlainTextEdit#logBody { background:#172a43; color:#d4e0f1; border:0; border-radius:0; padding:16px; font-family:Consolas,"DejaVu Sans Mono",monospace; font-size:12px; selection-background-color:#44668e; }
QCheckBox { spacing:8px; color:#5b708a; }
QCheckBox::indicator { width:15px; height:15px; }
QCheckBox#flowOption { padding:10px 12px; border-radius:6px; }
QCheckBox#flowOption:checked { background:#f0f5ff; color:#385f9a; }
QProgressBar { background:#edf1f7; border:0; border-radius:2px; }
QProgressBar::chunk { background:#6b95ed; border-radius:2px; }
QLabel#statusBadge { border-radius:6px; padding:4px 8px; font-size:10px; font-weight:600; background:#edf2f9; color:#7286a1; }
QLabel#statusBadge[tone="green"] { background:#e9f6ef; color:#188365; }
QLabel#statusBadge[tone="red"] { background:#fceef0; color:#c14450; }
QLabel#statusBadge[tone="blue"] { background:#edf3ff; color:#3169e9; }
QLabel#statusBadge[tone="amber"] { background:#fff6e5; color:#a36c12; }
QLabel#toast { background:white; color:#46617e; border:1px solid #cbdcf5; border-left:4px solid #6b94e6; border-radius:9px; padding:12px 16px; }
QDialog,QMessageBox { background:white; }
QDialogButtonBox { padding-top:10px; }
QGroupBox { border:1px solid #e5ebf3; border-radius:9px; margin-top:12px; padding:18px 10px 10px; }
QGroupBox::title { subcontrol-origin:margin; left:10px; padding:0 6px; }
QScrollArea { border:0; background:white; }
QWidget#formContent,QWidget#flowContents { background:white; }
QSplitter::handle { background:transparent; width:16px; height:12px; }
QScrollBar:vertical { background:transparent; width:9px; margin:2px; }
QScrollBar::handle:vertical { background:#cbd7e7; border-radius:4px; min-height:30px; }
QScrollBar:horizontal { background:transparent; height:9px; margin:2px; }
QScrollBar::handle:horizontal { background:#cbd7e7; border-radius:4px; min-width:30px; }
QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical { height:0; }
QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal { width:0; }
QScrollBar::add-page,QScrollBar::sub-page { background:transparent; }
QToolTip { background:#233b5a; color:white; border:0; padding:8px; }
)QSS");
}

class NumberedLog : public QPlainTextEdit {
    class Gutter : public QWidget {
    public:
        explicit Gutter(NumberedLog* owner)
            : QWidget(owner)
            , owner_(owner) {
        }

        void paintEvent(QPaintEvent* event) override {
            owner_->paintNumbers(event);
        }

    private:
        NumberedLog* owner_;
    };

public:
    explicit NumberedLog(QWidget* parent = nullptr)
        : QPlainTextEdit(parent)
        , gutter_(new Gutter(this)) {
        connect(this, &QPlainTextEdit::blockCountChanged, this, [this] {
            updateMargin();
        });
        connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect& rect, int dy) {
            if (dy)
                gutter_->scroll(0, dy);
            else
                gutter_->update(0, rect.y(), gutter_->width(), rect.height());
        });
        updateMargin();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QPlainTextEdit::resizeEvent(event);
        gutter_->setGeometry(
            contentsRect().left(), contentsRect().top(), gutterWidth(), contentsRect().height());
    }

private:
    int gutterWidth() const {
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
        const int digit = fontMetrics().horizontalAdvance(QLatin1Char('9'));
#else
        const int digit = fontMetrics().width(QLatin1Char('9'));
#endif
        return 18 + digit * QString::number(qMax(1, blockCount())).size();
    }

    void updateMargin() {
        setViewportMargins(gutterWidth(), 0, 0, 0);
        gutter_->setGeometry(
            contentsRect().left(), contentsRect().top(), gutterWidth(), contentsRect().height());
    }

    void paintNumbers(QPaintEvent* event) {
        QPainter painter(gutter_);
        painter.fillRect(event->rect(), QColor("#15243a"));
        painter.setPen(QColor("#7e96b7"));
        painter.setFont(font());
        auto block = firstVisibleBlock();
        int number = block.blockNumber();
        int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
        while (block.isValid() && top <= event->rect().bottom()) {
            const int height = qRound(blockBoundingRect(block).height());
            if (block.isVisible() && top + height >= event->rect().top())
                painter.drawText(0,
                    top,
                    gutter_->width() - 8,
                    fontMetrics().height(),
                    Qt::AlignRight,
                    QString::number(number + 1));
            top += height;
            block = block.next();
            ++number;
        }
    }

    Gutter* gutter_;
};

inline void toast(QWidget* root, const QString& text, bool error) {
    auto label = new QLabel(root);
    label->setObjectName("toast");
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setText(text);
    if (error)
        label->setStyleSheet("border-left-color:#c14450;");
    label->setFixedWidth(420);
    label->adjustSize();
    const auto target = QRect(root->width() - label->width() - 24,
        root->height() - label->height() - 24,
        label->width(),
        label->height());
    label->setGeometry(target.translated(0, 6));
    label->show();
    label->raise();
    auto move = new QPropertyAnimation(label, "geometry", label);
    move->setDuration(160);
    move->setStartValue(label->geometry());
    move->setEndValue(target);
    move->setEasingCurve(QEasingCurve::OutCubic);
    move->start(QAbstractAnimation::DeleteWhenStopped);
    QTimer::singleShot(4500, label, &QObject::deleteLater);
}

inline QWidget* badge(const QString& value) {
    auto holder = new QWidget;
    auto row = new QHBoxLayout(holder);
    row->setContentsMargins(6, 4, 6, 4);
    auto label = new QLabel(value);
    label->setObjectName("statusBadge");
    label->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    label->setMinimumWidth(35);
    label->setProperty("tone",
        value == "PASS" || value == "ONLINE"   ? "green"
        : value == "FAIL" || value == "FAILED" ? "red"
        : value == "RUNNING"                   ? "blue"
        : value == "TIMEOUT"                   ? "amber"
                                               : "neutral");
    row->addWidget(label);
    row->addStretch();
    return holder;
}

// Paint-only click feedback: does not move controls or block real RPC actions.
class ClickPulse : public QWidget {
public:
    explicit ClickPulse(QWidget* parent)
        : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setGeometry(parent->rect());
        show();
        raise();
        auto animation = new QVariantAnimation(this);
        animation->setDuration(180);
        animation->setStartValue(0.0);
        animation->setEndValue(1.0);
        connect(animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            progress_ = value.toReal();
            update();
        });
        connect(animation, &QVariantAnimation::finished, this, &QObject::deleteLater);
        animation->start();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(36, 95, 193, int(32 * (1 - progress_))));
        p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 6, 6);
    }

private:
    qreal progress_{0};
};

class InteractionFeedback : public QObject {
public:
    explicit InteractionFeedback(QWidget* root)
        : QObject(root)
        , root_(root) {
        qApp->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* target, QEvent* event) override {
        if (event->type() == QEvent::Show && root_ && qApp->platformName() != "offscreen" &&
            qgetenv("GCRP_REDUCED_MOTION") != "1") {
            auto dialog = qobject_cast<QDialog*>(target);
            if (dialog && root_->isAncestorOf(dialog)) {
                auto fade = new QPropertyAnimation(dialog, "windowOpacity", dialog);
                fade->setDuration(160);
                fade->setStartValue(0.0);
                fade->setEndValue(1.0);
                fade->start(QAbstractAnimation::DeleteWhenStopped);
            }
        }
        auto button = qobject_cast<QPushButton*>(target);
        if (button && root_ && root_->isAncestorOf(button) && button->isEnabled() &&
            qgetenv("GCRP_REDUCED_MOTION") != "1" && event->type() == QEvent::MouseButtonRelease) {
            auto mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton && button->rect().contains(mouse->pos()))
                new ClickPulse(button);
        }
        return false;
    }

private:
    QPointer<QWidget> root_;
};

inline QWidget* card(QWidget* content) {
    auto panel = new QFrame;
    panel->setObjectName("contentCard");
    auto layout = new QVBoxLayout(panel);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->addWidget(content);
    return panel;
}

// Fade a captured old page above the new page. Applying opacity to the entire
// stack nests effects around the cards' shadows and breaks Qt5 raster painting.
class PageTransition : public QObject {
public:
    PageTransition(QStackedWidget* pages, QObject* owner)
        : QObject(owner)
        , pages_(pages) {
        overlay_ = new QLabel(pages);
        overlay_->setAttribute(Qt::WA_TransparentForMouseEvents);
        overlay_->hide();
        opacity_ = new QGraphicsOpacityEffect(overlay_);
        overlay_->setGraphicsEffect(opacity_);
        animation_ = new QPropertyAnimation(opacity_, "opacity", this);
        animation_->setDuration(180);
        animation_->setStartValue(1.0);
        animation_->setEndValue(0.0);
        animation_->setEasingCurve(QEasingCurve::OutCubic);
        connect(animation_, &QPropertyAnimation::finished, this, [this] {
            overlay_->hide();
        });
    }

    void capture() {
        animation_->stop();
        overlay_->hide();
        overlay_->setPixmap(pages_->grab());
        overlay_->setGeometry(pages_->rect());
    }

    void start() {
        if (qgetenv("GCRP_REDUCED_MOTION") == "1")
            return;
        overlay_->show();
        overlay_->raise();
        opacity_->setOpacity(1.0);
        animation_->start();
    }

private:
    QStackedWidget* pages_;
    QLabel* overlay_;
    QGraphicsOpacityEffect* opacity_;
    QPropertyAnimation* animation_;
};

inline PageTransition* pageTransition(QStackedWidget* pages, QObject* owner) {
    return new PageTransition(pages, owner);
}

inline QIcon navigationIcon(int page) {
    QPixmap pixmap(40, 40);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor("#527caf"), 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (page == 0) {
        painter.drawRoundedRect(QRectF(6, 7, 28, 11), 3, 3);
        painter.drawRoundedRect(QRectF(6, 23, 28, 11), 3, 3);
        painter.drawPoint(QPointF(11, 12));
        painter.drawPoint(QPointF(11, 28));
        painter.drawLine(QPointF(24, 12), QPointF(29, 12));
        painter.drawLine(QPointF(24, 28), QPointF(29, 28));
    } else if (page == 1) {
        for (int y : {10, 20, 30}) {
            painter.drawRoundedRect(QRectF(6, y - 3, 6, 6), 1, 1);
            painter.drawLine(QPointF(18, y), QPointF(33, y));
        }
    } else if (page == 2) {
        painter.drawRoundedRect(QRectF(5, 11, 30, 23), 3, 3);
        painter.drawLine(QPointF(7, 11), QPointF(7, 6));
        painter.drawLine(QPointF(7, 6), QPointF(17, 6));
        painter.drawLine(QPointF(17, 6), QPointF(22, 11));
        painter.drawLine(QPointF(20, 17), QPointF(20, 28));
        painter.drawLine(QPointF(15, 22), QPointF(25, 22));
    } else if (page == 4) {
        painter.drawRect(QRectF(7, 7, 26, 26));
        for (int position : {14, 20, 26}) {
            painter.drawLine(QPointF(position, 7), QPointF(position, 33));
            painter.drawLine(QPointF(7, position), QPointF(33, position));
        }
    } else {
        painter.drawLine(QPointF(11, 7), QPointF(11, 33));
        painter.drawLine(QPointF(29, 7), QPointF(29, 33));
        painter.drawLine(QPointF(11, 14), QPointF(29, 25));
        painter.drawEllipse(QPointF(11, 7), 3, 3);
        painter.drawEllipse(QPointF(11, 33), 3, 3);
        painter.drawEllipse(QPointF(29, 7), 3, 3);
        painter.drawEllipse(QPointF(29, 33), 3, 3);
    }
    painter.end();
    return QIcon(pixmap);
}
} // namespace GcrpUi
