#include "core/storedreader.h"

#include <QFile>
#include <QFileInfo>
#include <QObject>

#include <new>

StoredReader storedFileReader(const QString &path, qint64 fileSize, const QDateTime &modified)
{
    return [path, fileSize, modified](qint64 offset, qint64 size) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() != fileSize || QFileInfo(path).lastModified() != modified
            || offset < 0 || size < 0 || offset > fileSize - size || !file.seek(offset))
            return QByteArray();
        QByteArray bytes = file.read(size);
        return bytes.size() == size ? bytes : QByteArray();
    };
}

StoredReader storedBytesReader(QByteArray data)
{
    return [data = std::move(data)](qint64 offset, qint64 size) {
        if (offset < 0 || size < 0 || offset > data.size() - size)
            return QByteArray();
        return data.mid(qsizetype(offset), qsizetype(size));
    };
}

QString fileTooLargeMessage(qint64 maxSize)
{
    constexpr qint64 kGb = qint64{1024} * 1024 * 1024;
    if (maxSize >= kGb && maxSize % kGb == 0)
        return QObject::tr("File is larger than %1 GB.").arg(maxSize / kGb);
    return QObject::tr("File is larger than %1 MB.").arg(maxSize / (1024 * 1024));
}

std::optional<QByteArray> readWholeFile(const QString &path, qint64 maxSize, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return std::nullopt;
    }
    if (file.size() > maxSize) {
        if (error)
            *error = fileTooLargeMessage(maxSize);
        return std::nullopt;
    }
    try {
        return file.readAll();
    } catch (const std::bad_alloc &) {
        if (error)
            *error = QObject::tr("Not enough memory to open this file.");
        return std::nullopt;
    }
}
