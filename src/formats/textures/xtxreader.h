#pragma once

#include "core/format.h"
#include "textures/texturedecode.h"

#include <optional>
#include <vector>

struct XtxTextureInfo
{
    qsizetype at = 0;
    quint64 dataSize = 0;
    quint32 width = 0, height = 0, depth = 0, target = 0, format = 0, mipCount = 0, sliceSize = 0;
    quint32 blockHeightLog2 = 0, arrayLength = 0;
};

struct XtxTexture
{
    XtxTextureInfo info;
    qsizetype start = 0;
    qsizetype size = 0;
};

struct XtxLayout
{
    std::vector<XtxTexture> textures;
    int infoBlocks = 0;
};

class XtxReader : public FormatReader
{
public:
    static constexpr qint64 kMaxFileSize = qint64{512} * 1024 * 1024;
    static constexpr int kMaxSlices = 64;

    QString name() const override { return QStringLiteral("Switch texture (XTX)"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override
    {
        Q_UNUSED(fileName);
        return fromBytes(data, error);
    }

    static std::unique_ptr<Document> fromBytes(const QByteArray &data, QString *error);
};

std::optional<PixelFormat> xtxPixelFormat(quint32 nvn);
std::optional<XtxLayout> parseXtxLayout(const QByteArray &data, QString *error);
