#include "core/storedreader.h"

#include <QFile>
#include <QFileInfo>

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
