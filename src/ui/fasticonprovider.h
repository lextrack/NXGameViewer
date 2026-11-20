#pragma once

#include <QApplication>
#include <QFileIconProvider>
#include <QStyle>

class FastIconProvider : public QFileIconProvider
{
public:
    QIcon icon(const QFileInfo &info) const override { return info.isDir() ? m_folder : m_file; }

    QString type(const QFileInfo &info) const override
    {
        if (info.isDir())
            return QStringLiteral("Folder");
        const QString suffix = info.suffix();
        return suffix.isEmpty() ? QStringLiteral("File") : suffix.toUpper() + QStringLiteral(" file");
    }

private:
    QIcon m_folder = QApplication::style()->standardIcon(QStyle::SP_DirIcon);
    QIcon m_file = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
};
