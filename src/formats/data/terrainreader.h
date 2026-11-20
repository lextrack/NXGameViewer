#pragma once

#include "core/format.h"

#include <optional>
#include <vector>

struct TerrainTile
{
    int grid = 0;
    int margin = 0;
    int layers = 0;
    bool rearranged = false;
    QByteArray tiled;
};

std::optional<TerrainTile> parseTerrainTile(const QByteArray &file, QString *error);
std::optional<std::vector<QByteArray>> terrainLayers(const TerrainTile &tile, int bytesPerTexel);

class TerrainReader : public FormatReader
{
public:
    static constexpr qint64 kMaxFileSize = qint64{64} * 1024 * 1024;

    QString name() const override { return QStringLiteral("Terrain (TARB, TSCB)"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;
};
