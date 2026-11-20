#include "cri/crireader.h"

#include "core/contentkind.h"
#include "core/simpletable.h"
#include "cri/criutf.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QObject>
#include <QSet>
#include <QtEndian>

namespace {

struct Afs2Entry
{
    int id = 0;
    qsizetype offset = 0;
    qsizetype size = 0;
};

std::optional<std::vector<Afs2Entry>> readAfs2(QByteArrayView data, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<std::vector<Afs2Entry>> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (data.size() < 16 || !data.startsWith("AFS2"))
        return fail(QObject::tr("Not a CRI AFS2 archive."));
    const auto *raw = reinterpret_cast<const uchar *>(data.data());
    const int offsetSize = raw[5];
    const int idSize = qFromLittleEndian<quint16>(raw + 6);
    const quint32 count = qFromLittleEndian<quint32>(raw + 8);
    const int alignment = std::max(1, int(qFromLittleEndian<quint16>(raw + 12)));
    if ((offsetSize != 2 && offsetSize != 4 && offsetSize != 8) || (idSize != 2 && idSize != 4) || count > 1'000'000)
        return fail(QObject::tr("The AFS2 header is not valid."));
    const qsizetype ids = 16, offsets = ids + qsizetype(count) * idSize;
    if (offsets + qsizetype(count + 1) * offsetSize > data.size())
        return fail(QObject::tr("The AFS2 file list is cut short."));
    auto offsetAt = [&](quint32 i) -> qint64 {
        const uchar *p = raw + offsets + qsizetype(i) * offsetSize;
        return offsetSize == 2 ? qFromLittleEndian<quint16>(p) : offsetSize == 4 ? qFromLittleEndian<quint32>(p) : qint64(qFromLittleEndian<quint64>(p));
    };
    std::vector<Afs2Entry> entries;
    for (quint32 i = 0; i < count; ++i) {
        const uchar *idp = raw + ids + qsizetype(i) * idSize;
        Afs2Entry e;
        e.id = idSize == 2 ? qFromLittleEndian<quint16>(idp) : int(qFromLittleEndian<quint32>(idp));
        const qint64 start = (offsetAt(i) + alignment - 1) / alignment * alignment, end = offsetAt(i + 1);
        if (start > end || end > data.size())
            return fail(QObject::tr("Sound %1 points outside the archive.").arg(e.id));
        e.offset = qsizetype(start);
        e.size = qsizetype(end - start);
        entries.push_back(e);
    }
    return entries;
}

std::optional<CriUtfTable> subTable(const CriUtfTable &table, const QString &column)
{
    const QByteArray bytes = table.value(0, column).toByteArray();
    if (bytes.isEmpty())
        return std::nullopt;
    return readCriUtf(bytes, nullptr);
}

struct AcbNames
{
    QHash<int, QString> memory;
    QHash<int, QString> stream;
};

class AcbResolver
{
public:
    explicit AcbResolver(const CriUtfTable &acb)
    {
        m_cues = subTable(acb, QStringLiteral("CueTable"));
        m_waveforms = subTable(acb, QStringLiteral("WaveformTable"));
        m_synths = subTable(acb, QStringLiteral("SynthTable"));
        m_sequences = subTable(acb, QStringLiteral("SequenceTable"));
        m_tracks = subTable(acb, QStringLiteral("TrackTable"));
        m_events = subTable(acb, QStringLiteral("TrackEventTable"));
        if (!m_events)
            m_events = subTable(acb, QStringLiteral("CommandTable"));
    }

    void cue(int index, const QString &name, AcbNames &out)
    {
        if (!m_cues)
            return;
        const int type = m_cues->value(index, QStringLiteral("ReferenceType")).toInt();
        const int target = m_cues->value(index, QStringLiteral("ReferenceIndex")).toInt();
        follow(type, target, name, out, 0);
    }

private:
    void follow(int type, int index, const QString &name, AcbNames &out, int depth)
    {
        if (depth > 8)
            return;
        switch (type) {
        case 1: waveform(index, name, out); break;
        case 2: synth(index, name, out, depth); break;
        case 3: sequence(index, name, out, depth); break;
        default: break;
        }
    }

    void waveform(int index, const QString &name, AcbNames &out)
    {
        if (!m_waveforms || index < 0 || size_t(index) >= m_waveforms->rows.size())
            return;
        const int streaming = m_waveforms->value(index, QStringLiteral("Streaming")).toInt();
        auto add = [&](QHash<int, QString> &names, const QVariant &id) {
            if (!id.isValid())
                return;
            const int key = id.toInt();
            if (key == 0xFFFF || names.contains(key))
                return;
            names.insert(key, name);
        };
        const QVariant legacy = m_waveforms->value(index, QStringLiteral("Id"));
        if (streaming == 0 || streaming == 2)
            add(out.memory, m_waveforms->column(QStringLiteral("MemoryAwbId")) >= 0
                                ? m_waveforms->value(index, QStringLiteral("MemoryAwbId")) : legacy);
        if (streaming == 1 || streaming == 2)
            add(out.stream, m_waveforms->column(QStringLiteral("StreamAwbId")) >= 0
                                ? m_waveforms->value(index, QStringLiteral("StreamAwbId")) : legacy);
    }

    void synth(int index, const QString &name, AcbNames &out, int depth)
    {
        if (!m_synths)
            return;
        const QByteArray items = m_synths->value(index, QStringLiteral("ReferenceItems")).toByteArray();
        for (qsizetype at = 0; at + 4 <= items.size(); at += 4)
            follow(qFromBigEndian<quint16>(items.constData() + at), qFromBigEndian<quint16>(items.constData() + at + 2), name,
                   out, depth + 1);
    }

    void sequence(int index, const QString &name, AcbNames &out, int depth)
    {
        if (!m_sequences || !m_tracks || !m_events)
            return;
        const QByteArray tracks = m_sequences->value(index, QStringLiteral("TrackIndex")).toByteArray();
        for (qsizetype t = 0; t + 2 <= tracks.size(); t += 2) {
            const int track = qFromBigEndian<quint16>(tracks.constData() + t);
            const int event = m_tracks->value(track, QStringLiteral("EventIndex")).toInt();
            const QByteArray commands = m_events->value(event, QStringLiteral("Command")).toByteArray();
            for (qsizetype at = 0; at + 3 <= commands.size();) {
                const int code = qFromBigEndian<quint16>(commands.constData() + at);
                const int size = quint8(commands[at + 2]);
                const qsizetype payload = at + 3;
                if (payload + size > commands.size())
                    break;
                if ((code == 2000 || code == 2003) && size >= 4)
                    follow(qFromBigEndian<quint16>(commands.constData() + payload),
                           qFromBigEndian<quint16>(commands.constData() + payload + 2), name, out, depth + 1);
                at = payload + size;
            }
        }
    }

    std::optional<CriUtfTable> m_cues, m_waveforms, m_synths, m_sequences, m_tracks, m_events;
};

AcbNames acbNames(const CriUtfTable &acb)
{
    AcbNames out;
    const std::optional<CriUtfTable> names = subTable(acb, QStringLiteral("CueNameTable"));
    if (!names)
        return out;
    AcbResolver resolver(acb);
    for (int r = 0; r < int(names->rows.size()); ++r)
        resolver.cue(names->value(r, QStringLiteral("CueIndex")).toInt(), names->value(r, QStringLiteral("CueName")).toString(),
                     out);
    return out;
}

QString extensionFor(QByteArrayView head)
{
    if (head.size() >= 4 && quint8(head[0]) == 0x01 && quint8(head[1]) == 0x00 && quint8(head[2]) == 0x00 && quint8(head[3]) == 0x80)
        return QStringLiteral(".opus");
    if (head.size() >= 3 && (quint8(head[0]) & 0x7F) == 'H' && (quint8(head[1]) & 0x7F) == 'C' && (quint8(head[2]) & 0x7F) == 'A')
        return QStringLiteral(".hca");
    if (head.size() >= 2 && quint8(head[0]) == 0x80 && quint8(head[1]) == 0x00)
        return QStringLiteral(".adx");
    if (head.startsWith("@UTF"))
        return QStringLiteral(".utf");
    if (head.startsWith("AFS2"))
        return QStringLiteral(".awb");
    return QStringLiteral(".bin");
}

class ArchiveTable : public SimpleTable
{
public:
    ArchiveTable(QString name, std::vector<SimpleColumn> columns, std::vector<Row> rows, std::shared_ptr<const Archive> archive)
        : SimpleTable(std::move(name), std::move(columns), std::move(rows)), m_archive(std::move(archive))
    {
    }
    std::shared_ptr<const Archive> archive() const override { return m_archive; }

private:
    std::shared_ptr<const Archive> m_archive;
};

std::shared_ptr<Table> soundsTable(const QByteArray &awb, const QHash<int, QString> &names, const QString &title, QString *error)
{
    const std::optional<std::vector<Afs2Entry>> entries = readAfs2(awb, error);
    if (!entries)
        return nullptr;
    auto archive = std::make_shared<Archive>();
    archive->data = awb;
    std::vector<SimpleTable::Row> rows;
    QSet<QString> used;
    int named = 0;
    for (const Afs2Entry &e : *entries) {
        const QByteArrayView head = QByteArrayView(awb).sliced(e.offset, std::min<qsizetype>(e.size, 16));
        QString base = names.value(e.id);
        if (!base.isEmpty())
            ++named;
        else
            base = QStringLiteral("%1").arg(e.id, 4, 10, QLatin1Char('0'));
        QString name = base + extensionFor(head);
        for (int n = 2; used.contains(name); ++n)
            name = QStringLiteral("%1_%2%3").arg(base).arg(n).arg(extensionFor(head));
        used.insert(name);
        archive->entries.push_back({name, e.offset, e.size});
        rows.push_back({name, e.id, qint64(e.size), describeContent(head)});
    }
    auto table = std::make_shared<ArchiveTable>(
        title,
        std::vector<SimpleColumn>{{QObject::tr("name"), false, QObject::tr("The cue name from the .acb file, or the sound's ID")},
                                  {QObject::tr("id")},
                                  {QObject::tr("size")},
                                  {QObject::tr("type")}},
        std::move(rows), archive);
    QString description = QObject::tr("%n sound(s)", nullptr, int(entries->size()));
    if (named > 0)
        description += QObject::tr(", %n named from the cue list", nullptr, named);
    table->setDescription(description);
    return table;
}

QString cellText(const QVariant &v)
{
    if (v.typeId() == QMetaType::QByteArray) {
        const QByteArray b = v.toByteArray();
        if (b.isEmpty())
            return {};
        return QObject::tr("%1 (%2)").arg(QLocale().formattedDataSize(b.size()), describeContent(QByteArrayView(b).first(std::min<qsizetype>(b.size(), 16))));
    }
    return v.toString();
}

std::unique_ptr<Document> utfDocument(const CriUtfTable &utf, const QString &path)
{
    auto doc = std::make_unique<Document>();
    const bool isAcb = utf.column(QStringLiteral("CueNameTable")) >= 0;
    if (isAcb) {
        const AcbNames names = acbNames(utf);
        const QByteArray memory = utf.value(0, QStringLiteral("AwbFile")).toByteArray();
        if (!memory.isEmpty())
            if (auto table = soundsTable(memory, names.memory, QObject::tr("Sounds"), nullptr))
                doc->tables.push_back(std::move(table));
        if (!path.isEmpty()) {
            const QFileInfo info(path);
            const QString awb = QDir(info.absolutePath()).filePath(info.completeBaseName() + QStringLiteral(".awb"));
            QFile file(awb);
            if (file.open(QIODevice::ReadOnly))
                if (auto table = soundsTable(file.readAll(), names.stream, QObject::tr("Streamed sounds (%1)").arg(QFileInfo(awb).fileName()), nullptr))
                    doc->tables.push_back(std::move(table));
        }
    }

    std::vector<SimpleColumn> columns;
    for (const CriUtfColumn &c : utf.columns)
        columns.push_back({c.name, false, criUtfTypeName(c.type) + (c.perRow ? QString() : QObject::tr(", same in every row"))});
    std::vector<SimpleTable::Row> rows;
    auto archive = std::make_shared<Archive>();
    std::vector<SimpleTable::Row> dataRows;
    for (size_t r = 0; r < utf.rows.size(); ++r) {
        SimpleTable::Row row;
        for (size_t c = 0; c < utf.columns.size(); ++c) {
            const QVariant &v = utf.rows[r][c];
            if (v.typeId() == QMetaType::QByteArray) {
                row.push_back(cellText(v));
                const QByteArray bytes = v.toByteArray();
                if (!bytes.isEmpty()) {
                    const QByteArrayView head = QByteArrayView(bytes).first(std::min<qsizetype>(bytes.size(), 16));
                    const QString name = (utf.rows.size() == 1 ? utf.columns[c].name
                                                               : QStringLiteral("%1[%2]").arg(utf.columns[c].name).arg(r))
                                       + extensionFor(head);
                    archive->entries.push_back({name, archive->data.size(), bytes.size()});
                    archive->data += bytes;
                    dataRows.push_back({name, qint64(bytes.size()), describeContent(head)});
                }
            } else {
                row.push_back(v);
            }
        }
        rows.push_back(std::move(row));
    }
    auto main = std::make_shared<SimpleTable>(utf.name.isEmpty() ? QObject::tr("Table") : utf.name, std::move(columns), std::move(rows));
    main->setDescription(QObject::tr("CRI table %1: %n row(s)", nullptr, int(utf.rows.size())).arg(utf.name));
    doc->tables.push_back(std::move(main));
    if (!archive->entries.empty()) {
        auto embedded = std::make_shared<ArchiveTable>(
            QObject::tr("Embedded data"),
            std::vector<SimpleColumn>{{QObject::tr("name")}, {QObject::tr("size")}, {QObject::tr("type")}}, std::move(dataRows),
            archive);
        embedded->setDescription(QObject::tr("%n data cell(s); double-click to open one", nullptr, int(archive->entries.size())));
        doc->tables.push_back(std::move(embedded));
    }
    return doc;
}

QByteArray readAll(const QString &path, qint64 limit, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return {};
    }
    if (file.size() > limit) {
        if (error)
            *error = QObject::tr("The file is too large.");
        return {};
    }
    return file.readAll();
}

}

int CriUtfReader::score(const QString &, QByteArrayView head) const
{
    return head.startsWith("@UTF") ? 100 : 0;
}

std::unique_ptr<Document> CriUtfReader::read(const QString &path, QString *error) const
{
    const QByteArray data = readAll(path, kMaxFileSize, error);
    if (data.isEmpty())
        return nullptr;
    const std::optional<CriUtfTable> utf = readCriUtf(data, error);
    return utf ? utfDocument(*utf, path) : nullptr;
}

std::unique_ptr<Document> CriUtfReader::readBytes(const QByteArray &data, const QString &, QString *error) const
{
    const std::optional<CriUtfTable> utf = readCriUtf(data, error);
    return utf ? utfDocument(*utf, {}) : nullptr;
}

int Afs2Reader::score(const QString &, QByteArrayView head) const
{
    return head.startsWith("AFS2") ? 100 : 0;
}

std::unique_ptr<Document> Afs2Reader::read(const QString &path, QString *error) const
{
    const QByteArray data = readAll(path, kMaxFileSize, error);
    if (data.isEmpty())
        return nullptr;
    QHash<int, QString> names;
    const QFileInfo info(path);
    const QByteArray acb = readAll(QDir(info.absolutePath()).filePath(info.completeBaseName() + QStringLiteral(".acb")),
                                   CriUtfReader::kMaxFileSize, nullptr);
    if (const std::optional<CriUtfTable> utf = acb.isEmpty() ? std::nullopt : readCriUtf(acb, nullptr))
        names = acbNames(*utf).stream;
    auto table = soundsTable(data, names, QObject::tr("Sounds"), error);
    if (!table)
        return nullptr;
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(table));
    return doc;
}

std::unique_ptr<Document> Afs2Reader::readBytes(const QByteArray &data, const QString &, QString *error) const
{
    auto table = soundsTable(data, {}, QObject::tr("Sounds"), error);
    if (!table)
        return nullptr;
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(table));
    return doc;
}
