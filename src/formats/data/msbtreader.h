#pragma once

#include "core/format.h"

#include <optional>
#include <vector>

struct MsbtMessage
{
    int index = 0;
    QString label;
    QString text;
    QByteArray attribute;
};

class MsbtReader : public FormatReader
{
public:
    static constexpr qint64 kMaxFileSize = qint64{64} * 1024 * 1024;

    QString name() const override { return QStringLiteral("MSBT messages"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;

    static std::optional<std::vector<MsbtMessage>> parse(const QByteArray &data, QString *error = nullptr);
};
