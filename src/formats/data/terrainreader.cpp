#include "data/terrainreader.h"

#include "archives/decompress.h"
#include "core/simpletable.h"
#include "core/storedreader.h"
#include "textures/texturedecode.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QObject>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace {

constexpr qsizetype kInfoSize = 0x34;
constexpr int kMaxGrid = 4096;

template <typename T>
std::optional<T> fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return std::nullopt;
}

QString kindOf(const QString &fileName)
{
    QString name = QFileInfo(fileName).fileName().toLower();
    if (name.endsWith(QLatin1String(".zs")))
        name.chop(3);
    const qsizetype dot = name.indexOf(u'.');
    return dot < 0 ? QString() : name.mid(dot + 1);
}

Image fromGrey(const std::vector<float> &values, int side)
{
    Image image{side, side, QByteArray(qsizetype(side) * side * 4, '\xff')};
    float low = 0, high = 0;
    bool any = false;
    for (const float v : values)
        if (std::isfinite(v)) {
            low = any ? std::min(low, v) : v;
            high = any ? std::max(high, v) : v;
            any = true;
        }
    for (size_t i = 0; i < values.size(); ++i) {
        const float v = values[i];
        const int grey = std::isfinite(v) && high > low ? int(std::lround(255.0 * (v - low) / (high - low))) : 0;
        image.rgba[qsizetype(i) * 4] = char(grey);
        image.rgba[qsizetype(i) * 4 + 1] = char(grey);
        image.rgba[qsizetype(i) * 4 + 2] = char(grey);
    }
    return image;
}

std::array<uchar, 3> materialColor(int index)
{
    const quint32 h = quint32(index + 1) * 2654435761u;
    return {uchar(64 + (h & 0x7F) + ((h >> 7) & 0x3F)), uchar(64 + ((h >> 13) & 0x7F) + ((h >> 20) & 0x3F)),
            uchar(64 + ((h >> 26) & 0x3F) + ((h >> 3) & 0x7F))};
}

constexpr int kNoMaterial = 127;

Image materialImage(const QByteArray &indices, const QByteArray &weights, int side)
{
    Image image{side, side, QByteArray(qsizetype(side) * side * 4, '\xff')};
    for (qsizetype i = 0; i < qsizetype(side) * side; ++i) {
        std::array<int, 3> sum{};
        int total = 0;
        for (int k = 0; k < 4; ++k) {
            const int index = uchar(indices[i * 4 + k]);
            const int weight = weights.isEmpty() ? (k == 0 ? 255 : 0) : uchar(weights[i * 4 + k]);
            if (index == kNoMaterial || weight == 0)
                continue;
            const auto color = materialColor(index);
            for (int c = 0; c < 3; ++c)
                sum[size_t(c)] += color[size_t(c)] * weight;
            total += weight;
        }
        for (int c = 0; c < 3; ++c)
            image.rgba[i * 4 + c] = char(total ? sum[size_t(c)] / total : 0);
    }
    return image;
}

std::unique_ptr<Document> readScene(const QByteArray &d, QString *error)
{
    if (d.size() < 0x74) {
        if (error)
            *error = QObject::tr("The terrain scene is cut short.");
        return nullptr;
    }
    auto i32 = [&d](qsizetype at) { return at >= 0 && at + 4 <= d.size() ? qFromLittleEndian<qint32>(d.constData() + at) : 0; };
    auto text = [&](qsizetype field) {
        const qint32 offset = i32(field);
        const qsizetype at = field + offset;
        if (offset == 0 || at < 0 || at >= d.size())
            return QString();
        const qsizetype end = d.indexOf('\0', at);
        return end < 0 ? QString() : QString::fromLatin1(d.constData() + at, end - at);
    };
    const quint32 version = qFromLittleEndian<quint32>(d.constData() + 4);
    const float worldScale = std::bit_cast<float>(quint32(i32(0x10)));
    const float maxHeight = std::bit_cast<float>(quint32(i32(0x14)));
    std::vector<SimpleTable::Row> rows;
    QJsonArray json;
    const int count = std::clamp(i32(0x30), 0, 64);
    const qsizetype table = 0x70 + i32(0x70);
    for (int i = 0; i < count && i32(0x70) != 0; ++i) {
        const qsizetype field = table + qsizetype(i) * 4;
        const qsizetype e = field + i32(field);
        if (e < 0 || e + 0x70 > d.size() || uchar(d[e + 1]) == 0xE0)
            continue;
        const QString name = text(e + 0x60), extension = text(e + 0x64), resource = text(e + 0x6C);
        const int width = i32(e + 0x10), margin = i32(e + 0x14), layers = i32(e + 0x18), mips = i32(e + 0x1C);
        const int format = qFromLittleEndian<quint16>(d.constData() + e + 0x20);
        rows.push_back({name, extension, resource, width, margin, layers, mips, QStringLiteral("0x%1").arg(format, 2, 16, QLatin1Char('0'))});
        json.append(QJsonObject{{"name", name}, {"extension", extension}, {"resource", resource}, {"width", width},
                                {"margin", margin}, {"layers", layers}, {"mips", mips}, {"format", format}});
    }
    auto result = std::make_shared<SimpleTable>(
        QObject::tr("Terrain layers"),
        std::vector<SimpleColumn>{{QObject::tr("layer")}, {QObject::tr("file")}, {QObject::tr("resource")},
                                  {QObject::tr("width")}, {QObject::tr("margin")}, {QObject::tr("array layers")},
                                  {QObject::tr("mips")}, {QObject::tr("format"), true}},
        std::move(rows));
    result->setDescription(QObject::tr("Terrain scene, version 0x%1: world scale %2, maximum height %3. Its tiles are the "
                                       "files in the folder of the same name.")
                               .arg(version, 0, 16)
                               .arg(double(worldScale))
                               .arg(double(maxHeight)));
    const QJsonObject all{{"version", QStringLiteral("0x%1").arg(version, 0, 16)}, {"world_scale", double(worldScale)},
                          {"max_height", double(maxHeight)}, {"layers", json}};
    result->setJsonExporter([all] { return QJsonDocument(all).toJson(); });
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(result));
    return doc;
}

}

std::optional<TerrainTile> parseTerrainTile(const QByteArray &file, QString *error)
{
    if (file.size() < 0x14 || !file.startsWith("TARB"))
        return fail<TerrainTile>(error, QObject::tr("Not a terrain tile (no \"TARB\" at the start)."));
    const int headerSize = qFromLittleEndian<quint16>(file.constData() + 0x0A);
    const qint64 compressed = qFromLittleEndian<qint32>(file.constData() + 0x10);
    if (headerSize < 0x14 || headerSize > file.size() || compressed < 0 || compressed > file.size() - headerSize)
        return fail<TerrainTile>(error, QObject::tr("The terrain tile header is damaged."));
    QByteArray payload;
    if (compressed) {
        const std::optional<QByteArray> plain = decompress(file.mid(headerSize, compressed), error);
        if (!plain)
            return std::nullopt;
        payload = *plain;
    } else {
        payload = file.mid(headerSize);
    }
    if (payload.size() < kInfoSize)
        return fail<TerrainTile>(error, QObject::tr("The terrain tile is cut short."));
    TerrainTile tile;
    tile.grid = uchar(payload[0]) << 8 | uchar(payload[1]);
    tile.margin = uchar(payload[2]);
    tile.layers = std::max(1, int(uchar(payload[3])));
    tile.rearranged = (uchar(payload[8]) & 0x80) != 0;
    if (tile.grid <= 0 || tile.grid > kMaxGrid)
        return fail<TerrainTile>(error, QObject::tr("The terrain tile has an impossible size (%1).").arg(tile.grid));
    const qint32 offset = qFromLittleEndian<qint32>(payload.constData() + 0x2C);
    const qint32 size = qFromLittleEndian<qint32>(payload.constData() + 0x30);
    if (offset != 0 && size > 0 && 0x2C + qint64(offset) + size <= payload.size())
        tile.tiled = payload.mid(0x2C + offset, size);
    return tile;
}

std::optional<std::vector<QByteArray>> terrainLayers(const TerrainTile &tile, int bytesPerTexel)
{
    const int blockHeight = defaultBlockHeight(tile.grid);
    const qsizetype layerSize = blockLinearSize(tile.grid, tile.grid, bytesPerTexel, blockHeight);
    if (layerSize <= 0 || layerSize * tile.layers != tile.tiled.size())
        return std::nullopt;
    std::vector<QByteArray> layers;
    for (int l = 0; l < tile.layers; ++l) {
        QByteArray swizzled = tile.tiled.mid(layerSize * l, layerSize);
        swapGobSectors(swizzled);
        const std::optional<QByteArray> linear = deswizzleBlockLinear(swizzled, tile.grid, tile.grid, bytesPerTexel, blockHeight);
        if (!linear)
            return std::nullopt;
        layers.push_back(*linear);
    }
    return layers;
}

int TerrainReader::score(const QString &, QByteArrayView head) const
{
    if (head.startsWith("TARB"))
        return 100;
    return head.startsWith("TSCB") ? 100 : 0;
}

std::unique_ptr<Document> TerrainReader::read(const QString &path, QString *error) const
{
    const std::optional<QByteArray> bytes = readWholeFile(path, kMaxFileSize, error);
    if (!bytes)
        return nullptr;
    return readBytes(*bytes, path, error);
}

std::unique_ptr<Document> TerrainReader::readBytes(const QByteArray &data, const QString &fileName, QString *error) const
{
    if (data.startsWith("TSCB"))
        return readScene(data, error);
    const std::optional<TerrainTile> tile = parseTerrainTile(data, error);
    if (!tile)
        return nullptr;
    const QString kind = kindOf(fileName);
    const int side = tile->grid;
    std::vector<SimpleTable::Row> rows = {
        {QObject::tr("kind"), kind.isEmpty() ? QObject::tr("unknown") : kind},
        {QObject::tr("size"), QObject::tr("%1 × %1 (%2 inside a %3-texel margin)").arg(side).arg(side - 2 * tile->margin).arg(tile->margin)},
        {QObject::tr("array layers"), tile->layers},
        {QObject::tr("GPU data"), QLocale().formattedDataSize(tile->tiled.size())},
    };
    auto doc = std::make_unique<Document>();
    QString description;
    std::shared_ptr<SimpleTable> info;
    auto addLayer = [&](const QString &name, Image image, const QString &text) {
        auto table = std::make_shared<SimpleTable>(name, std::vector<SimpleColumn>{{QObject::tr("property")}, {QObject::tr("value")}},
                                                   rows);
        table->setImage(std::make_shared<Image>(std::move(image)));
        table->setDescription(text);
        doc->tables.push_back(table);
        return table;
    };

    const bool fourBytes = !kind.contains(QLatin1String("bake_"));
    const std::optional<std::vector<QByteArray>> layers = fourBytes ? terrainLayers(*tile, 4) : std::nullopt;
    if (layers && (kind == QLatin1String("hght") || kind.endsWith(QLatin1String(".hght")))) {
        const QByteArray &layer = layers->front();
        std::vector<float> heights(size_t(side) * size_t(side));
        for (size_t i = 0; i < heights.size(); ++i)
            heights[i] = std::bit_cast<float>(qFromLittleEndian<quint32>(layer.constData() + qsizetype(i) * 4));
        const auto [low, high] = std::minmax_element(heights.begin(), heights.end());
        rows.push_back({QObject::tr("height range"), QObject::tr("%1 – %2 (fraction of the scene's maximum height)")
                                                         .arg(double(*low), 0, 'g', 6)
                                                         .arg(double(*high), 0, 'g', 6)});
        auto table = addLayer(QObject::tr("Height map"), fromGrey(heights, side),
                              QObject::tr("Heights from low (black) to high (white) in this tile"));
        table->setJsonExporter([heights, side] {
            QJsonArray values;
            for (const float h : heights)
                values.append(double(h));
            return QJsonDocument(QJsonObject{{"size", side}, {"heights", values}}).toJson(QJsonDocument::Compact);
        });
        return doc;
    }
    if (layers && (kind == QLatin1String("mate") || kind.endsWith(QLatin1String(".mate")))) {
        QSet<int> used;
        const QByteArray &indices = layers->front();
        for (qsizetype i = 0; i < indices.size(); ++i)
            if (uchar(indices[i]) != kNoMaterial)
                used.insert(uchar(indices[i]));
        QList<int> sorted = used.values();
        std::sort(sorted.begin(), sorted.end());
        QStringList names;
        for (const int m : sorted)
            names << QString::number(m);
        rows.push_back({QObject::tr("materials used"), names.join(QStringLiteral(", "))});
        addLayer(QObject::tr("Materials"), materialImage(indices, layers->size() > 1 ? (*layers)[1] : QByteArray(), side),
                 QObject::tr("Up to four materials per texel, blended by weight; each colour stands for one material "
                             "index of the scene"));
        return doc;
    }
    if (layers) {
        for (size_t l = 0; l < layers->size(); ++l)
            addLayer(layers->size() > 1 ? QObject::tr("Layer %1").arg(l + 1) : QObject::tr("Terrain data"),
                     Image{side, side, (*layers)[l]}, QObject::tr("The raw RGBA values of this terrain layer"));
        return doc;
    }
    info = std::make_shared<SimpleTable>(QObject::tr("Terrain tile"),
                                         std::vector<SimpleColumn>{{QObject::tr("property")}, {QObject::tr("value")}},
                                         std::move(rows));
    info->setDescription(QObject::tr("Baked lighting or another compressed layer: its pixels cannot be shown yet"));
    doc->tables.push_back(std::move(info));
    return doc;
}
