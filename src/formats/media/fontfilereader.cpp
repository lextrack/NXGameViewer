#include "media/fontfilereader.h"

#include "core/simpletable.h"
#include "core/storedreader.h"
#include "data/nintendofont.h"

#include <QFile>
#include <QFileInfo>
#include <QGlyphRun>
#include <QImage>
#include <QObject>
#include <QPainter>
#include <QPainterPath>
#include <QRawFont>

#include <algorithm>
#include <cstring>

namespace {

constexpr int kColumns = 16;
constexpr int kCell = 64;
constexpr int kBanner = 72;
constexpr qreal kGlyphSize = 40;

class FontFilesTable : public SimpleTable
{
public:
    FontFilesTable(std::vector<Row> rows, std::shared_ptr<const Archive> archive)
        : SimpleTable(QObject::tr("Files"), {{QObject::tr("name")}, {QObject::tr("size")}, {QObject::tr("type")}},
                      std::move(rows)),
          m_archive(std::move(archive))
    {
    }

    std::shared_ptr<const Archive> archive() const override { return m_archive; }

private:
    std::shared_ptr<const Archive> m_archive;
};

QString plainName(const QString &fileName, const QByteArray &font)
{
    QString base = QFileInfo(fileName).fileName();
    for (const QLatin1String suffix : {QLatin1String(".zs"), QLatin1String(".bfttf"), QLatin1String(".bfotf")})
        if (base.endsWith(suffix, Qt::CaseInsensitive))
            base.chop(suffix.size());
    const qsizetype platform = base.indexOf(QLatin1String(".Nin_"));
    if (platform > 0)
        base.truncate(platform);
    if (base.isEmpty())
        base = QStringLiteral("font");
    return base + u'.' + sfntSuffix(font);
}

QString kindName(bool encrypted, const QByteArray &font)
{
    const QString suffix = sfntSuffix(font);
    if (encrypted)
        return suffix == QLatin1String("otf") ? QObject::tr("Nintendo font (BFOTF), OpenType inside")
                                              : QObject::tr("Nintendo font (BFTTF), TrueType inside");
    if (suffix == QLatin1String("ttc"))
        return QObject::tr("TrueType collection");
    return suffix == QLatin1String("otf") ? QObject::tr("OpenType") : QObject::tr("TrueType");
}

}

Image FontFileReader::renderPreview(const QByteArray &font, int glyphs)
{
    const QRawFont raw(font, kGlyphSize);
    if (!raw.isValid())
        return {};
    const int shown = std::clamp(glyphs, 1, kPreviewGlyphs);
    const int rows = (shown + kColumns - 1) / kColumns;
    QImage canvas(kColumns * kCell, kBanner + rows * kCell, QImage::Format_RGBA8888_Premultiplied);
    canvas.fill(Qt::white);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QColor(220, 220, 220));
    for (int c = 0; c <= kColumns; ++c)
        painter.drawLine(c * kCell, kBanner, c * kCell, kBanner + rows * kCell);
    for (int r = 0; r <= rows; ++r)
        painter.drawLine(0, kBanner + r * kCell, kColumns * kCell, kBanner + r * kCell);

    const QString sample = QStringLiteral("The quick brown fox jumps 0123456789");
    if (raw.supportsCharacter(u'A')) {
        const QList<quint32> indexes = raw.glyphIndexesForString(sample);
        QList<QPointF> advances = raw.advancesForGlyphIndexes(indexes);
        QList<QPointF> positions;
        QPointF pen(12, kBanner / 2 + raw.ascent() / 2 - raw.descent() / 2);
        for (const QPointF &advance : advances) {
            positions << pen;
            pen += advance;
        }
        QGlyphRun run;
        run.setRawFont(raw);
        run.setGlyphIndexes(indexes);
        run.setPositions(positions);
        painter.setPen(Qt::black);
        painter.drawGlyphRun(QPointF(), run);
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    for (int g = 0; g < shown; ++g) {
        const QPainterPath path = raw.pathForGlyph(quint32(g));
        if (path.isEmpty())
            continue;
        const QRectF bounds = path.boundingRect();
        const QRectF cell(g % kColumns * kCell, kBanner + g / kColumns * kCell, kCell, kCell);
        const qreal scale = std::min({1.0, (kCell - 10) / std::max<qreal>(bounds.width(), 1),
                                      (kCell - 10) / std::max<qreal>(bounds.height(), 1)});
        painter.save();
        painter.translate(cell.center());
        painter.scale(scale, scale);
        painter.translate(-bounds.center());
        painter.drawPath(path);
        painter.restore();
    }
    painter.end();

    const QImage rgba = canvas.convertToFormat(QImage::Format_RGBA8888);
    Image image{rgba.width(), rgba.height(), QByteArray(qsizetype(rgba.width()) * rgba.height() * 4, '\0')};
    for (int y = 0; y < rgba.height(); ++y)
        std::memcpy(image.rgba.data() + qsizetype(y) * rgba.width() * 4, rgba.constScanLine(y), size_t(rgba.width()) * 4);
    return image;
}

int FontFileReader::score(const QString &fileName, QByteArrayView head) const
{
    if (isNintendoFont(head))
        return 95;
    if (!isSfnt(head))
        return 0;
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    if (suffix == QLatin1String("ttf") || suffix == QLatin1String("otf") || suffix == QLatin1String("ttc"))
        return 90;
    return head.startsWith("OTTO") || head.startsWith("ttcf") || head.startsWith("true") ? 70 : 20;
}

std::unique_ptr<Document> FontFileReader::read(const QString &path, QString *error) const
{
    const std::optional<QByteArray> bytes = readWholeFile(path, kMaxFileSize, error);
    if (!bytes)
        return nullptr;
    return readBytes(*bytes, path, error);
}

std::unique_ptr<Document> FontFileReader::readBytes(const QByteArray &data, const QString &fileName, QString *error) const
{
    const bool encrypted = !isSfnt(data);
    const std::optional<QByteArray> decrypted = encrypted ? decryptNintendoFont(data) : std::optional<QByteArray>(data);
    const std::optional<SfntInfo> info = decrypted ? sfntInfo(*decrypted) : std::nullopt;
    if (!info) {
        if (error)
            *error = encrypted ? QObject::tr("Not a Nintendo font (BFTTF/BFOTF) this app can decode.")
                               : QObject::tr("The font's table directory is damaged.");
        return nullptr;
    }
    const QByteArray font = *decrypted;
    std::vector<SimpleTable::Row> rows = {
        {QObject::tr("kind"), kindName(encrypted, font)},
        {QObject::tr("family"), info->family},
        {QObject::tr("style"), info->style},
        {QObject::tr("full name"), info->fullName},
        {QObject::tr("version"), info->version},
        {QObject::tr("PostScript name"), info->postScriptName},
        {QObject::tr("outlines"), info->outlines},
        {QObject::tr("glyphs"), info->glyphs},
    };
    if (info->fonts > 1)
        rows.push_back({QObject::tr("fonts in the collection"), info->fonts});
    rows.push_back({QObject::tr("tables"), info->tables.join(QStringLiteral(", "))});

    auto table = std::make_shared<SimpleTable>(QObject::tr("Font"),
                                               std::vector<SimpleColumn>{{QObject::tr("property")}, {QObject::tr("value")}},
                                               std::move(rows));
    const int glyphs = info->glyphs;
    const auto maker = [font, glyphs] { return renderPreview(font, glyphs); };
    table->setImageMaker(maker);
    const QString name = info->fullName.isEmpty() ? info->family : info->fullName;
    table->setDescription(
        encrypted ? QObject::tr("%1: %n glyph(s). The plain font is listed as a file: extract it to install or edit it.",
                                nullptr, glyphs)
                        .arg(name)
                  : QObject::tr("%1: %n glyph(s)", nullptr, glyphs).arg(name));
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(table));
    if (encrypted) {
        auto archive = std::make_shared<Archive>();
        archive->data = font;
        const QString plain = plainName(fileName, font);
        archive->entries.push_back({plain, 0, font.size()});
        auto files = std::make_shared<FontFilesTable>(
            std::vector<SimpleTable::Row>{{plain, qint64(font.size()), kindName(false, font)}}, archive);
        files->setImageMaker(maker);
        files->setDescription(QObject::tr("The decoded font, ready to extract"));
        doc->tables.push_back(std::move(files));
    }
    return doc;
}
