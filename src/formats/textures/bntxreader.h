#pragma once

#include "core/format.h"
#include "textures/texturedecode.h"

#include <optional>
#include <vector>

struct BntxTexture
{
    QString name;
    qsizetype infoOffset = 0;
    quint32 format = 0;
    int width = 0;
    int height = 0;
    int depth = 0;
    int arrayLength = 0;
    int mipCount = 0;
    int blockHeightLog2 = 0;
    int tileMode = 0;
    int dimension = 0;
    bool swappedGobSectors = false;
    quint32 channels = 0;
    qsizetype imageSize = 0;
    std::vector<qsizetype> mipOffsets;
};

struct BntxFile
{
    qsizetype offset = 0;
    qsizetype size = 0;
    QString name;
    std::vector<BntxTexture> textures;
};

std::optional<BntxFile> parseBntx(const QByteArray &data, qsizetype offset, QString *error);
std::vector<qsizetype> findEmbeddedBntx(const QByteArray &data);
std::optional<PixelFormat> bntxPixelFormat(quint32 format);
QString bntxTextureProblem(const BntxTexture &texture);
std::optional<Image> decodeBntxTexture(const QByteArray &data, qsizetype bntxOffset, const BntxTexture &texture);
QString bntxFormatName(quint32 format);
QString channelSelectorText(quint32 channels);
void applyChannelSelectors(QByteArray &rgba, quint32 channels);
QByteArray unapplyChannelSelectors(const QByteArray &shown, const QByteArray &stored, quint32 channels);

class BntxReader : public FormatReader
{
public:
    static constexpr qint64 kMaxFileSize = qint64{512} * 1024 * 1024;
    static constexpr int kMaxSide = 16384;
    static constexpr int kMaxTextures = 10000;
    static constexpr int kMaxEmbedded = 64;

    QString name() const override { return QStringLiteral("Switch texture (BNTX)"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;
};
