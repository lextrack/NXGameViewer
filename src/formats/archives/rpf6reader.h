#pragma once

#include "core/format.h"

#include <QByteArray>
#include <QList>

#include <functional>
#include <optional>

class Rpf6Reader : public FormatReader
{
public:
    static constexpr quint32 kEncrypted = 0xFFFFFFFD;
    static constexpr qint64 kMaxNameTable = qint64{256} * 1024 * 1024;

    QString name() const override { return QStringLiteral("RAGE RPF6 archive"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;
    bool readsStored() const override { return true; }
    std::unique_ptr<Document> readStored(const StoredReader &read, qint64 size, const QString &fileName,
                                         QString *error) const override;

    static void setKeys(const QList<QByteArray> &keys);
    static QList<QByteArray> keys();
    static bool needsKey(const QString &path);
};

QByteArray rpf6Decrypt(const QByteArray &key, QByteArrayView data);
QByteArray rpf6Encrypt(const QByteArray &key, QByteArrayView data);
quint32 joaat(QByteArrayView name);
bool rpf6IsRootEntry(QByteArrayView decrypted);

std::optional<QByteArray> findRpf6Key(const QByteArray &executable, QByteArrayView firstEntry, QString *error,
                                      const std::function<bool(int percent)> &progress = {});
