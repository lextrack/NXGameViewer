#include "rage/ragetexturereader.h"

#include "core/simpletable.h"
#include "core/storedreader.h"
#include "rage/rageresource.h"
#include "textures/texturedecode.h"

#include <QObject>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

namespace {


struct Encoding
{
    std::optional<PixelFormat> format;
    std::optional<PixelMasks> masks;
    QByteArray fourCC;
    QString name;
    bool crunch = false;
};

Encoding encodingOf(quint32 code)
{
    Encoding e;
    const QByteArray text(reinterpret_cast<const char *>(&code), 4);
    struct Compressed { const char *fourCC; PixelFormat format; };
    static const Compressed compressed[] = {
        {"DXT1", PixelFormat::BC1}, {"DXT2", PixelFormat::BC2}, {"DXT3", PixelFormat::BC2}, {"DXT4", PixelFormat::BC3},
        {"DXT5", PixelFormat::BC3}, {"ATI1", PixelFormat::BC4}, {"ATI2", PixelFormat::BC5},
    };
    for (const Compressed &c : compressed)
        if (text == QByteArrayView(c.fourCC, 4)) {
            e.format = c.format;
            e.fourCC = text;
            e.name = pixelFormatName(c.format);
            return e;
        }
    switch (code) {
    case 21: e.masks = PixelMasks{32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000}; e.name = QStringLiteral("A8R8G8B8"); break;
    case 22: e.masks = PixelMasks{32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0}; e.name = QStringLiteral("X8R8G8B8"); break;
    case 26: e.masks = PixelMasks{16, 0x0F00, 0x00F0, 0x000F, 0xF000}; e.name = QStringLiteral("A4R4G4B4"); break;
    case 28: e.masks = PixelMasks{8, 0, 0, 0, 0xFF}; e.name = QStringLiteral("A8"); break;
    case 32: e.masks = PixelMasks{32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000}; e.name = QStringLiteral("A8B8G8R8"); break;
    case 50: e.masks = PixelMasks{8, 0xFF, 0, 0, 0, true}; e.name = QStringLiteral("L8"); break;
    case 51: e.masks = PixelMasks{16, 0x00FF, 0, 0, 0xFF00, true}; e.name = QStringLiteral("A8L8"); break;
    default:
        e.crunch = text == "CRND";
        e.name = e.crunch ? QObject::tr("Crunch (CRND)") : QObject::tr("format 0x%1").arg(code, 8, 16, QLatin1Char('0'));
        break;
    }
    return e;
}

qsizetype levelSize(const Encoding &e, int width, int height)
{
    if (e.format)
        return qsizetype(elementColumns(*e.format, width)) * elementRows(*e.format, height) * bytesPerElement(*e.format);
    if (e.masks)
        return qsizetype(width) * height * (e.masks->bits / 8);
    return 0;
}

struct Texture
{
    QString name;
    int width = 0;
    int height = 0;
    int mips = 1;
    Encoding encoding;
    QByteArray data;
};

QByteArray ddsFile(const Texture &t)
{
    QByteArray h(128, '\0');
    auto put = [&h](int at, quint32 v) { qToLittleEndian(v, h.data() + at); };
    std::memcpy(h.data(), "DDS ", 4);
    put(4, 124);
    put(8, 0x1007 | (t.mips > 1 ? 0x20000 : 0) | (t.encoding.format ? 0x80000 : 0x8));
    put(12, quint32(t.height));
    put(16, quint32(t.width));
    put(20, quint32(t.encoding.format ? levelSize(t.encoding, t.width, t.height)
                                      : qsizetype(t.width) * (t.encoding.masks->bits / 8)));
    put(28, quint32(t.mips));
    put(76, 32);
    if (t.encoding.format) {
        put(80, 0x4);
        std::memcpy(h.data() + 84, t.encoding.fourCC.constData(), 4);
    } else {
        const PixelMasks &m = *t.encoding.masks;
        put(80, (m.luminance ? 0x20000 : (m.r || m.g || m.b ? 0x40 : 0)) | (m.a ? (m.r || m.luminance ? 0x1 : 0x2) : 0));
        put(88, quint32(m.bits));
        put(92, m.r);
        put(96, m.g);
        put(100, m.b);
        put(104, m.a);
    }
    put(108, 0x1000 | (t.mips > 1 ? 0x400008 : 0));
    return h + t.data;
}

class ListTable : public SimpleTable
{
public:
    ListTable(std::vector<Row> rows, std::shared_ptr<const Archive> archive)
        : SimpleTable(QObject::tr("Textures"),
                      {{QObject::tr("name")}, {QObject::tr("size")}, {QObject::tr("format")}, {QObject::tr("mip levels")}},
                      std::move(rows)),
          m_archive(std::move(archive))
    {
    }
    std::shared_ptr<const Archive> archive() const override { return m_archive; }

private:
    std::shared_ptr<const Archive> m_archive;
};

QString cleanName(QString name)
{
    if (name.startsWith(QLatin1String("memory:")))
        name = name.mid(name.lastIndexOf(u':') + 1);
    return name;
}

std::optional<Texture> readTexture(const RageResource &r, qint64 at, int index)
{
    const std::optional<quint32> namePointer = r.u32(at + 24);
    const std::optional<quint16> width = r.u16(at + 32), height = r.u16(at + 34);
    const std::optional<quint32> code = r.u32(at + 36), pixels = r.u32(at + 76);
    const std::optional<qint64> pixelsAt = pixels ? r.offsetOf(*pixels) : std::nullopt;
    if (!width || !height || !code || !pixelsAt || *width == 0 || *height == 0 || *width > kMaxTextureSide || *height > kMaxTextureSide
        || !r.u32(at + 40))
        return std::nullopt;
    Texture t;
    t.name = namePointer ? cleanName(r.cstring(*namePointer)) : QString();
    if (t.name.isEmpty())
        t.name = QStringLiteral("texture %1").arg(index);
    t.width = *width;
    t.height = *height;
    t.encoding = encodingOf(*code);
    const int declaredMips = std::clamp(int(uchar(r.body[qsizetype(at + 43)])), 1, 16);
    qsizetype total = 0;
    t.mips = 0;
    for (int level = 0; level < declaredMips; ++level) {
        const qsizetype size = levelSize(t.encoding, std::max(1, t.width >> level), std::max(1, t.height >> level));
        if (size == 0 || *pixelsAt + total + size > r.body.size())
            break;
        total += size;
        ++t.mips;
    }
    if (t.mips > 0)
        t.data = r.body.mid(qsizetype(*pixelsAt), total);
    return t;
}

std::optional<std::vector<qint64>> dictionaryEntries(const RageResource &r, qint64 at)
{
    const std::optional<quint32> values = r.u32(at + 0x18);
    const std::optional<quint16> count = r.u16(at + 0x1C);
    const std::optional<qint64> valuesAt = values ? r.offsetOf(*values) : std::nullopt;
    if (!valuesAt || !count)
        return std::nullopt;
    std::vector<qint64> entries;
    for (int i = 0; i < *count; ++i) {
        const std::optional<quint32> pointer = r.u32(*valuesAt + 4 * i);
        const std::optional<qint64> entry = pointer ? r.offsetOf(*pointer) : std::nullopt;
        entries.push_back(entry ? *entry : -1);
    }
    return entries;
}

bool inVirtual(const RageResource &r, std::optional<quint32> pointer)
{
    const std::optional<qint64> at = pointer ? r.offsetOf(*pointer) : std::nullopt;
    return at && *at < r.virtualSize;
}

std::vector<Texture> findEmbeddedTextures(const RageResource &r)
{
    std::vector<Texture> textures;
    QSet<qint64> seen;
    for (qint64 at = 0; at + 0x20 <= r.virtualSize; at += 4) {
        if (r.u32(at + 8) != 0u || r.u32(at + 12) != 1u)
            continue;
        const quint16 count = r.u16(at + 0x14).value_or(0);
        if (count == 0 || count != r.u16(at + 0x1C) || r.u16(at + 0x16).value_or(0) < count
            || r.u16(at + 0x1E).value_or(0) < count || !inVirtual(r, r.u32(at + 0x10)) || !inVirtual(r, r.u32(at + 0x18)))
            continue;
        const std::optional<std::vector<qint64>> entries = dictionaryEntries(r, at);
        if (!entries)
            continue;
        std::vector<std::pair<qint64, Texture>> found;
        for (qint64 entry : *entries) {
            std::optional<Texture> t = entry >= 0 && entry < r.virtualSize && r.u32(entry + 24).has_value()
                                               && inVirtual(r, r.u32(entry + 24))
                                           ? readTexture(r, entry, int(textures.size() + found.size()))
                                           : std::nullopt;
            if (!t || (!t->encoding.format && !t->encoding.masks && !t->encoding.crunch)) {
                found.clear();
                break;
            }
            found.emplace_back(entry, std::move(*t));
        }
        for (auto &[entry, t] : found)
            if (!seen.contains(entry)) {
                seen.insert(entry);
                textures.push_back(std::move(t));
            }
    }
    return textures;
}

std::unique_ptr<Document> parse(const QByteArray &data, QString *error)
{
    auto fail = [error](const QString &message) -> std::unique_ptr<Document> {
        if (error)
            *error = message;
        return nullptr;
    };
    std::optional<RageResource> r = readRageResource(data, error);
    if (!r)
        return nullptr;
    std::vector<Texture> textures;
    int problems = 0;
    const bool dictionary = r->type == RageTextureDictionaryReader::kType;
    if (dictionary) {
        const std::optional<std::vector<qint64>> entries = dictionaryEntries(*r, 0);
        if (!entries)
            return fail(QObject::tr("The texture dictionary is damaged."));
        for (qint64 entry : *entries) {
            std::optional<Texture> t = entry >= 0 ? readTexture(*r, entry, int(textures.size() + problems)) : std::nullopt;
            if (t)
                textures.push_back(std::move(*t));
            else
                ++problems;
        }
    } else {
        textures = findEmbeddedTextures(*r);
        if (textures.empty())
            return fail(QObject::tr("This RAGE resource (type %1) has no textures the app can find.").arg(r->type));
    }

    auto archive = std::make_shared<Archive>();
    auto shared = std::make_shared<const std::vector<Texture>>(std::move(textures));
    std::vector<SimpleTable::Row> rows;
    std::vector<std::shared_ptr<const Table>> pictures;
    int unsupported = 0;
    for (const Texture &t : *shared) {
        const bool readable = t.mips > 0;
        unsupported += readable ? 0 : 1;
        rows.push_back({t.name, QStringLiteral("%1 × %2").arg(t.width).arg(t.height), t.encoding.name, t.mips});
        archive->entries.push_back({readable ? t.name.section(u'.', 0, 0) + QStringLiteral(".dds") : t.name, 0,
                                    readable ? 128 + t.data.size() : 0});

        auto table = std::make_shared<SimpleTable>(
            t.name, std::vector<SimpleColumn>{{QObject::tr("property")}, {QObject::tr("value")}},
            std::vector<SimpleTable::Row>{{QObject::tr("size"), QStringLiteral("%1 × %2").arg(t.width).arg(t.height)},
                                          {QObject::tr("format"), t.encoding.name},
                                          {QObject::tr("mip levels"), t.mips}});
        if (!readable) {
            table->setDescription(QObject::tr("Texture %1 in %2, which cannot be shown yet").arg(t.name, t.encoding.name));
        } else {
            table->setDescription(QObject::tr("Texture %1, %2 × %3 %4").arg(t.name).arg(t.width).arg(t.height).arg(t.encoding.name));
            table->setImageMaker([t] {
                const qsizetype size = levelSize(t.encoding, t.width, t.height);
                const QByteArrayView first = QByteArrayView(t.data).first(size);
                std::optional<QByteArray> rgba = t.encoding.format ? decodePixels(*t.encoding.format, first, t.width, t.height)
                                                                   : decodeMaskedPixels(first, t.width, t.height, *t.encoding.masks);
                Image image;
                if (rgba) {
                    image.width = t.width;
                    image.height = t.height;
                    image.rgba = std::move(*rgba);
                }
                return image;
            });
        }
        pictures.push_back(std::move(table));
    }
    archive->loader = [shared](size_t index) {
        if (index >= shared->size() || (*shared)[index].mips == 0)
            return QByteArray();
        return ddsFile((*shared)[index]);
    };
    auto list = std::make_shared<ListTable>(std::move(rows), archive);
    QString description = dictionary
                              ? QObject::tr("RAGE texture dictionary, %n texture(s)", nullptr, int(shared->size()))
                              : QObject::tr("RAGE resource (type %1) with %n embedded texture(s)", nullptr, int(shared->size()))
                                    .arg(r->type);
    if (unsupported)
        description += QObject::tr("; %n in formats that cannot be shown yet", nullptr, unsupported);
    if (problems)
        description += QObject::tr("; %n damaged entry(s) skipped", nullptr, problems);
    list->setDescription(description);
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(list));
    for (auto &p : pictures)
        doc->tables.push_back(std::move(p));
    return doc;
}

}

int RageTextureDictionaryReader::score(const QString &, QByteArrayView head) const
{
    if (!isRageResource(head))
        return 0;
    return qFromLittleEndian<quint32>(head.data() + 4) == kType ? 100 : 5;
}

std::unique_ptr<Document> RageTextureDictionaryReader::read(const QString &path, QString *error) const
{
    const std::optional<QByteArray> bytes = readWholeFile(path, kMaxFileSize, error);
    if (!bytes)
        return nullptr;
    return parse(*bytes, error);
}

std::unique_ptr<Document> RageTextureDictionaryReader::readBytes(const QByteArray &data, const QString &, QString *error) const
{
    return parse(data, error);
}
