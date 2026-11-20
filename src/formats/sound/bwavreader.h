#pragma once

#include "core/format.h"

#include <array>
#include <optional>
#include <vector>

struct BwavChannel
{
    int codec = 0;
    int sampleRate = 0;
    qint64 samples = 0;
    qint64 fullSamples = 0;
    qint64 start = 0;
    qint64 loopStart = -1;
    qint64 loopEnd = -1;
    std::array<qint16, 16> coefficients{};
    qint16 history1 = 0;
    qint16 history2 = 0;
};

struct BwavInfo
{
    bool prefetch = false;
    std::vector<BwavChannel> channels;

    qint64 dataEnd() const;
};

class BwavReader : public FormatReader
{
public:
    static constexpr qint64 kMaxFileSize = qint64{512} * 1024 * 1024;
    static constexpr int kMaxChannels = 16;

    QString name() const override { return QStringLiteral("Switch sound (BWAV)"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;

    static std::optional<BwavInfo> parse(QByteArrayView data, QString *error = nullptr);
    static std::optional<Audio> decode(QByteArrayView data, const BwavInfo &info, QString *error = nullptr);
};

std::vector<qint16> decodeDspAdpcm(QByteArrayView data, qint64 samples, const std::array<qint16, 16> &coefficients,
                                   qint16 history1, qint16 history2);
