#include "sound/barsreader.h"

#include "core/storedreader.h"
#include "sound/bwavreader.h"
#include "core/contentkind.h"
#include "core/simpletable.h"

#include <QObject>
#include <QSet>
#include <QtEndian>

#include <algorithm>

namespace {

class SoundPackTable : public SimpleTable
{
public:
    SoundPackTable(std::vector<Row> rows, std::shared_ptr<const Archive> archive)
        : SimpleTable(QObject::tr("Sounds"),
                      {{QObject::tr("name")}, {QObject::tr("size")}, {QObject::tr("length")}, {QObject::tr("type")}},
                      std::move(rows)),
          m_archive(std::move(archive))
    {
    }

    std::shared_ptr<const Archive> archive() const override { return m_archive; }

private:
    std::shared_ptr<const Archive> m_archive;
};

QString cString(const QByteArray &data, qsizetype at, qsizetype limit)
{
    if (at < 0 || at >= std::min(limit, data.size()))
        return {};
    const qsizetype stop = data.indexOf('\0', at);
    if (stop < 0 || stop > limit || stop - at > 255)
        return {};
    return QString::fromUtf8(data.constData() + at, stop - at);
}

QString amtaName(const QByteArray &data, qsizetype amta)
{
    if (amta < 0 || amta + 0x28 > data.size() || !QByteArrayView(data).sliced(amta).startsWith("AMTA"))
        return {};
    const qsizetype size = qFromLittleEndian<quint32>(data.constData() + amta + 8);
    const qsizetype limit = std::min<qsizetype>(data.size(), amta + std::max<qsizetype>(size, 0x28));
    const qsizetype field = amta + 0x24;
    QString name = cString(data, field + qFromLittleEndian<quint32>(data.constData() + field), limit);
    if (name.isEmpty()) {
        const qsizetype strg = data.indexOf("STRG", amta);
        if (strg > 0 && strg + 8 < limit)
            name = cString(data, strg + 8, limit);
    }
    return name;
}

QString duration(qint64 samples, int rate)
{
    if (rate <= 0)
        return {};
    const qint64 ms = samples * 1000 / rate;
    return QStringLiteral("%1:%2.%3")
        .arg(ms / 60000)
        .arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

}

int BarsReader::score(const QString &, QByteArrayView head) const
{
    return head.startsWith("BARS") ? 100 : 0;
}

std::unique_ptr<Document> BarsReader::read(const QString &path, QString *error) const
{
    const std::optional<QByteArray> bytes = readWholeFile(path, kMaxFileSize, error);
    if (!bytes)
        return nullptr;
    return readBytes(*bytes, path, error);
}

std::unique_ptr<Document> BarsReader::readBytes(const QByteArray &data, const QString &, QString *error) const
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return nullptr;
    };
    if (data.size() < 0x10 || !data.startsWith("BARS"))
        return fail(QObject::tr("Not a BARS sound pack (no \"BARS\" at the start)."));
    if (uchar(data[8]) != 0xFF || uchar(data[9]) != 0xFE)
        return fail(QObject::tr("Big-endian BARS files are not supported."));
    const quint32 count = qFromLittleEndian<quint32>(data.constData() + 12);
    const qsizetype table = 16 + qsizetype(count) * 4;
    if (count > quint32(kMaxEntries) || table + qsizetype(count) * 8 > data.size())
        return fail(QObject::tr("The BARS entry table does not fit in the file."));

    std::vector<qsizetype> starts;
    for (quint32 i = 0; i < count; ++i)
        starts.push_back(qFromLittleEndian<quint32>(data.constData() + table + qsizetype(i) * 8 + 4));
    std::vector<qsizetype> sorted = starts;
    std::sort(sorted.begin(), sorted.end());

    auto archive = std::make_shared<Archive>();
    archive->data = data;
    std::vector<SimpleTable::Row> rows;
    QSet<QString> used;
    int stubs = 0;
    for (quint32 i = 0; i < count; ++i) {
        const qsizetype amta = qFromLittleEndian<quint32>(data.constData() + table + qsizetype(i) * 8);
        const qsizetype start = starts[i];
        if (start <= 0 || start >= data.size())
            continue;
        const auto next = std::upper_bound(sorted.begin(), sorted.end(), start);
        qsizetype end = next != sorted.end() ? std::min<qsizetype>(*next, data.size()) : data.size();
        const QByteArrayView slice = QByteArrayView(data).sliced(start, end - start);
        QString length, type;
        const std::optional<BwavInfo> info = BwavReader::parse(slice);
        if (info) {
            end = std::min(end, start + info->dataEnd());
            const BwavChannel &c = info->channels.front();
            length = duration(c.fullSamples, c.sampleRate);
            const bool stored = c.samples > 0 && c.start < slice.size();
            type = stored ? QObject::tr("sound") : QObject::tr("description only (samples in another file)");
            stubs += stored ? 0 : 1;
        } else {
            type = describeContent(slice.first(std::min<qsizetype>(slice.size(), 16)));
        }
        QString name = amtaName(data, amta);
        if (name.isEmpty())
            name = QStringLiteral("%1").arg(qFromLittleEndian<quint32>(data.constData() + 16 + qsizetype(i) * 4), 8, 16,
                                            QLatin1Char('0'));
        QString unique = name;
        for (int n = 2; used.contains(unique); ++n)
            unique = QStringLiteral("%1_%2").arg(name).arg(n);
        used.insert(unique);
        ArchiveEntry entry;
        entry.name = unique + (info ? QStringLiteral(".bwav") : QStringLiteral(".bin"));
        entry.offset = start;
        entry.size = end - start;
        rows.push_back({entry.name, qint64(entry.size), length, type});
        archive->entries.push_back(std::move(entry));
    }

    auto list = std::make_shared<SoundPackTable>(std::move(rows), archive);
    QString description = QObject::tr("%n sound(s)", nullptr, int(archive->entries.size()));
    if (stubs > 0)
        description += QObject::tr("; %n only describe a sound stored elsewhere", nullptr, stubs);
    list->setDescription(description);
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(list));
    return doc;
}
