#pragma once
#include "MainWindow.h"
#include "Theme.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QSplitter>
#include <QGroupBox>
#include <QHeaderView>
#include <QFile>
#include <QFileDialog>
#include <QSaveFile>
#include <QMessageBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSignalBlocker>
#include <QTreeWidgetItemIterator>
#include <QUuid>
#include <QDateTime>
#include <QGridLayout>
#include <QProgressBar>
#include <QScrollArea>
#include <QDir>
#include <QTextCursor>
#include <QTextEdit>
#include <QTextCharFormat>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QApplication>
#include <QEventLoop>
#include <algorithm>
#include <cstdio>

namespace {
QString text(const Json& row, const char* key) {
    if (!row.is_object() || !row.contains(key) || row[key].is_null()) return QStringLiteral("—");
    const auto value = row[key].is_string() ? row[key].get<std::string>() : row[key].dump();
    return QString::fromStdString(value);
}
QWidget* page(const QString& title, const QString& subtitle) {
    auto widget = new QWidget;
    auto layout = new QVBoxLayout(widget);
    layout->setContentsMargins(28,24,28,20); layout->setSpacing(16);
    auto label = new QLabel(title); label->setTextFormat(Qt::PlainText); label->setObjectName("pageTitle");
    layout->addWidget(label);
    auto description = new QLabel(subtitle); description->setWordWrap(true);
    description->setObjectName("muted"); layout->addWidget(description);
    return widget;
}
QTableWidget* table(const QStringList& headers) {
    auto widget = new QTableWidget(0, headers.size());
    widget->setHorizontalHeaderLabels(headers);
    widget->setEditTriggers(QAbstractItemView::NoEditTriggers);
    widget->setSelectionBehavior(QAbstractItemView::SelectRows);
    widget->setSelectionMode(QAbstractItemView::SingleSelection);
    widget->setAlternatingRowColors(true);
    widget->verticalHeader()->hide(); widget->verticalHeader()->setDefaultSectionSize(44);
    widget->setShowGrid(false); widget->setMouseTracking(true);
    widget->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    widget->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft|Qt::AlignVCenter);
    return widget;
}
QString outcome(const Json& task) {
    const auto state=text(task,"status"),business=text(task.value("execution",Json::object()),"case_status");
    if(state=="SUCCEEDED")return business=="PASS"?QStringLiteral("PASS"):QStringLiteral("UNKNOWN");
    if(state=="FAILED")return business=="FAIL"?QStringLiteral("FAIL"):business=="CRASH"?QStringLiteral("CRASH"):QStringLiteral("ERROR");
    return state;
}
QString csvCell(QString value) {
    if(!value.isEmpty()&&QString("=+-@\t\r").contains(value[0]))value.prepend('\'');
    value.replace('"',"\"\"");return "\""+value+"\"";
}
void cell(QTableWidget* table, int row, int column, const QString& value) {
    auto item = new QTableWidgetItem(value);
    if(value.startsWith("ONLINE")||value=="PASS"||value==QStringLiteral("新通过"))item->setForeground(QColor("#287a61"));
    else if(value=="FAIL"||value=="FAILED"||value==QStringLiteral("新回退"))item->setForeground(QColor("#b44058"));
    else if(value=="TIMEOUT"||value=="DRAINING")item->setForeground(QColor("#987022"));
    else if(value=="OFFLINE")item->setForeground(QColor("#8795aa"));
    table->setItem(row,column,item);
}
QStringList selectedCases(QTreeWidget* tree) {
    QStringList paths;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        const auto path = (*it)->data(0,Qt::UserRole).toString();
        if (!path.isEmpty() && (*it)->checkState(0) == Qt::Checked) paths.append(path);
    }
    return paths;
}
QPushButton* pendingButton(const QString& label) {
    auto button = new QPushButton(label+QStringLiteral(" · 待接入"));
    button->setEnabled(false);
    button->setToolTip(QStringLiteral("当前 Server 没有对应 Client API"));
    return button;
}
}
