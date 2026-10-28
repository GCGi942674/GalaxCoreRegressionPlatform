#pragma once
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QTextCodec>
#include <QMap>
#include <QSet>
#include <stdexcept>

// Shared by manual submission and scheduled plans. Preserve order, remove duplicates.
inline QStringList parseCaseList(const QByteArray& bytes, const QSet<QString>& available) {
    auto fail = [](const QString& message) {
        throw std::runtime_error(message.toUtf8().constData());
    };
    if (bytes.size() > 8 * 1024 * 1024)
        fail(QStringLiteral("测试案例列表不能超过 8 MiB。"));
    QTextCodec::ConverterState state;
    auto content =
        QTextCodec::codecForName("UTF-8")->toUnicode(bytes.constData(), bytes.size(), &state);
    if (state.invalidChars || state.remainingChars)
        fail(QStringLiteral("文件不是有效的 UTF-8 文本，请转换编码后重试。"));
    if (content.startsWith(QChar(0xfeff)))
        content.remove(0, 1);
    content.replace("\r\n", "\n");
    content.replace('\r', '\n');
    if (available.isEmpty())
        fail(QStringLiteral("请先从 Server 加载测试案例目录。"));
    QSet<QString> selected;
    QStringList ordered;
    auto lines = content.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        auto path = lines[i].trimmed();
        if (path.isEmpty())
            continue;
        path.replace('\\', '/');
        while (path.startsWith("./"))
            path.remove(0, 2);
        if (!available.contains(path))
            fail(QStringLiteral("第 %1 行无法匹配 Server 测试案例：%2")
                     .arg(i + 1)
                     .arg(path.left(300)));
        if (!selected.contains(path)) {
            selected.insert(path);
            ordered.append(path);
        }
    }
    if (selected.isEmpty())
        fail(QStringLiteral("文件中没有测试案例路径。每行应为一个相对于 test2 的 run.tcl 路径。"));
    return ordered;
}

// Validate the entire list before changing selection. No partial imports.
inline int importCaseList(QTreeWidget* tree, const QByteArray& bytes) {
    QMap<QString, QTreeWidgetItem*> available;
    QSet<QString> paths;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        auto path = (*it)->data(0, Qt::UserRole).toString();
        if (!path.isEmpty()) {
            available[path] = *it;
            paths.insert(path);
        }
    }
    const auto ordered = parseCaseList(bytes, paths);
    QSet<QString> selected;
    for (const auto& path : ordered)
        selected.insert(path);
    // Successful import replaces selection, including currently filtered-out leaves.
    for (auto it = available.cbegin(); it != available.cend(); ++it) {
        const bool checked = selected.contains(it.key());
        it.value()->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
        if (checked)
            for (auto parent = it.value()->parent(); parent; parent = parent->parent())
                parent->setExpanded(true);
    }
    return selected.size();
}
