#pragma once
#include <QString>

// Match the shared console's light palette; keep daily layout/badges local.
inline QString dailyStyle() {
    return QStringLiteral(R"QSS(
QWidget { color:#172b48; background:#f5f7fb; font-size:13px; }
QLabel { background:transparent; }
QLabel#pageTitle { color:#172b48; font-size:25px; }
QLabel#muted, QLabel#dailySummary { color:#65758d; font-size:11px; }
QLabel#dailySync { color:#3169e9; font-size:12px; }
QLabel#dailyHeading { color:#172b48; font-size:15px; font-weight:600; }
QFrame#dailyPanel { background:#ffffff; border:1px solid #dce4ee; border-radius:10px; }
QPushButton { background:#ffffff; color:#344963; border:1px solid #dce4ee; border-radius:7px; padding:8px 12px; }
QPushButton:hover { background:#f2f6fd; border-color:#bccce5; }
QPushButton:pressed { background:#e5edfc; }
QPushButton:checked { background:#edf3ff; color:#2459c8; border-color:#8caef4; }
QPushButton:disabled { background:#f5f7fb; color:#a7b2c2; border-color:#e5ebf3; }
QPushButton#dailyMetric { background:#ffffff; text-align:left; border-radius:10px; min-height:90px; }
QPushButton#dailyMetric:checked { background:#edf3ff; border-color:#3169e9; }
QLabel#dailyMetricValue { color:#172b48; font-size:27px; font-weight:600; }
QLineEdit,QComboBox,QDateEdit { background:#ffffff; color:#172b48; border:1px solid #dce4ee; border-radius:7px; padding:8px 10px; selection-background-color:#dce8ff; }
QComboBox QAbstractItemView { background:#ffffff; color:#172b48; selection-background-color:#edf3ff; }
QTableWidget { background:#ffffff; alternate-background-color:#ffffff; color:#172b48; border:0; gridline-color:#dce4ee; selection-background-color:#edf3ff; selection-color:#172b48; }
QTableWidget::item { border-bottom:1px solid #edf1f7; padding:10px 8px; }
QTableWidget::item:hover { background:#f2f6fd; }
QHeaderView::section { background:#ffffff; color:#65758d; border:0; border-bottom:1px solid #dce4ee; padding:12px 8px; font-size:11px; }
QTableCornerButton::section { background:#ffffff; border:0; }
QWidget#dailyCell { background:transparent; }
QLabel#dailyBadge { border-radius:9px; padding:3px 8px; font-size:11px; font-weight:600; color:#52657e; background:#eef2f7; border:1px solid #dce4ee; }
QLabel#dailyBadge[tone="green"] { color:#187b59; background:#eaf7ef; border-color:#b9dfc9; }
QLabel#dailyBadge[tone="red"] { color:#b53344; background:#fff0f2; border-color:#edc4cb; }
QLabel#dailyBadge[tone="amber"] { color:#94620d; background:#fff7e6; border-color:#ead39f; }
QPushButton#dailyLogLink { background:transparent; border:0; color:#3169e9; padding:6px; }
QPushButton#dailyLogLink:hover { color:#3169e9; background:#edf3ff; }
QPlainTextEdit { color:#243650; background:#f8faff; border:1px solid #dce4ee; border-radius:7px; padding:10px; font-family:"Consolas","DejaVu Sans Mono",monospace; font-size:12px; }
QScrollBar:vertical { background:#ffffff; width:10px; }
QScrollBar::handle:vertical { background:#bccce5; min-height:24px; border-radius:4px; }
QSplitter::handle { background:#dce4ee; width:6px; }
)QSS");
}
