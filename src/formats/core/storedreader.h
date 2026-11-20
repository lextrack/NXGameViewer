#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>

#include <functional>
#include <optional>

using StoredReader = std::function<QByteArray(qint64 offset, qint64 size)>;

StoredReader storedFileReader(const QString &path, qint64 fileSize, const QDateTime &modified);
StoredReader storedBytesReader(QByteArray data);

QString fileTooLargeMessage(qint64 maxSize);
std::optional<QByteArray> readWholeFile(const QString &path, qint64 maxSize, QString *error);
