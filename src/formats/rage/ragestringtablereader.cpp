#include "rage/ragestringtablereader.h"

#include "archives/rpf6reader.h"
#include "core/simpletable.h"
#include "core/storedreader.h"
#include "rage/rageresource.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <optional>
#include <vector>

namespace {

constexpr quint32 kMaxSlots = 1 << 20;
constexpr quint32 kMaxStrings = 1 << 22;
constexpr int kMaxStringBytes = 1 << 20;
constexpr int kMaxLanguages = 32;

QString shown(QString text)
{
    return text.replace(u'\n', QStringLiteral(" ↵ "));
}

QString hashName(quint32 hash)
{
    return QStringLiteral("0x%1").arg(hash, 8, 16, QLatin1Char('0'));
}

QString languageFromName(const QString &fileName)
{
    static const QHash<QString, QString> names{
        {QStringLiteral("en"), QObject::tr("English")},
        {QStringLiteral("es"), QObject::tr("Spanish")},
        {QStringLiteral("fr"), QObject::tr("French")},
        {QStringLiteral("de"), QObject::tr("German")},
        {QStringLiteral("it"), QObject::tr("Italian")},
        {QStringLiteral("jp"), QObject::tr("Japanese")},
        {QStringLiteral("ja"), QObject::tr("Japanese")},
        {QStringLiteral("cht"), QObject::tr("Chinese (Traditional)")},
        {QStringLiteral("chs"), QObject::tr("Chinese (Simplified)")},
        {QStringLiteral("ko"), QObject::tr("Korean")},
        {QStringLiteral("mx"), QObject::tr("Spanish (Latin America)")},
        {QStringLiteral("bp"), QObject::tr("Portuguese (Brazil)")},
        {QStringLiteral("pt"), QObject::tr("Portuguese")},
        {QStringLiteral("pl"), QObject::tr("Polish")},
        {QStringLiteral("ru"), QObject::tr("Russian")},
    };
    const QString base = QFileInfo(fileName).completeBaseName().toLower();
    const qsizetype cut = std::max(base.lastIndexOf(u'-'), base.lastIndexOf(u'_'));
    return cut < 0 ? QString() : names.value(base.mid(cut + 1));
}

QString languageAt(int index)
{
    switch (index) {
    case 0: return QObject::tr("English");
    case 1: return QObject::tr("Spanish");
    case 2: return QObject::tr("French");
    case 3: return QObject::tr("German");
    case 4: return QObject::tr("Italian");
    case 5: return QObject::tr("Japanese");
    case 6: return QObject::tr("Chinese (Traditional)");
    case 7: return QObject::tr("Chinese (Simplified)");
    case 8: return QObject::tr("Korean");
    case 9:
    case 10: return QObject::tr("Spanish (Latin America)");
    case 11: return QObject::tr("Portuguese (Brazil)");
    case 12: return QObject::tr("Polish");
    case 13: return QObject::tr("Russian");
    default: return QObject::tr("language %1").arg(index);
    }
}

struct HashedString
{
    quint32 hash = 0;
    QString text;
};

bool looksLikeStringTable(QByteArrayView body)
{
    if (body.size() < 20)
        return false;
    const quint32 table = qFromLittleEndian<quint32>(body.data() + 8);
    return (table >> 28) == 5 && qFromLittleEndian<quint32>(body.data() + 12) == 0
           && qFromLittleEndian<quint32>(body.data() + 16) == 0;
}

std::optional<std::vector<HashedString>> parseResource(const QByteArray &data, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<std::vector<HashedString>> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    std::optional<RageResource> r = readRageResource(data, error);
    if (!r)
        return std::nullopt;
    if (r->type != RageStringTableReader::kType || !looksLikeStringTable(r->body))
        return fail(QObject::tr("This RAGE resource is not a string table."));
    const std::optional<qint64> tableAt = r->offsetOf(*r->u32(8));
    const std::optional<quint32> slotTotal = tableAt ? r->u32(*tableAt) : std::nullopt;
    const std::optional<quint32> slotArray = tableAt ? r->u32(*tableAt + 4) : std::nullopt;
    const std::optional<quint16> slotCount = tableAt ? r->u16(*tableAt + 8) : std::nullopt;
    const std::optional<quint32> total = tableAt ? r->u32(*tableAt + 12) : std::nullopt;
    const std::optional<qint64> slotsAt = slotArray ? r->offsetOf(*slotArray) : std::nullopt;
    if (!slotTotal || !slotsAt || !slotCount || !total || *slotTotal == 0 || *slotTotal > kMaxSlots || *slotCount != (*slotTotal & 0xFFFF)
        || *total > kMaxStrings)
        return fail(QObject::tr("The string table is damaged."));

    std::vector<HashedString> strings;
    QSet<quint32> visited;
    for (quint32 s = 0; s < *slotTotal; ++s) {
        std::optional<quint32> entry = r->u32(*slotsAt + 4 * qint64(s));
        if (!entry)
            return fail(QObject::tr("The string table is damaged."));
        while (*entry != 0) {
            const std::optional<qint64> at = r->offsetOf(*entry);
            if (!at || visited.contains(*entry) || strings.size() >= *total)
                return fail(QObject::tr("The string table is damaged."));
            visited.insert(*entry);
            const std::optional<quint32> hash = r->u32(*at), dataPointer = r->u32(*at + 4), next = r->u32(*at + 8);
            const std::optional<qint64> dataAt = dataPointer ? r->offsetOf(*dataPointer) : std::nullopt;
            const std::optional<quint32> textPointer = dataAt ? r->u32(*dataAt + 4) : std::nullopt;
            const std::optional<qint64> textAt = textPointer ? r->offsetOf(*textPointer) : std::nullopt;
            if (!hash || !next || !textAt)
                return fail(QObject::tr("The string table is damaged."));
            const qsizetype end = r->body.indexOf('\0', qsizetype(*textAt));
            if (end < 0 || end - *textAt > kMaxStringBytes)
                return fail(QObject::tr("The string table is damaged."));
            strings.push_back({*hash, QString::fromUtf8(r->body.constData() + *textAt, end - qsizetype(*textAt))});
            entry = next;
        }
    }
    return strings;
}

struct LanguageBlock
{
    int index = 0;
    QHash<quint32, QString> texts;
    QHash<quint32, QString> notes;
    std::vector<quint32> order;
};

struct LanguageTable
{
    QStringList identifiers;
    std::vector<LanguageBlock> languages;
};

class Cursor
{
public:
    explicit Cursor(const QByteArray &data) : m_data(data) {}
    bool fits(qint64 size) const { return size >= 0 && m_at <= m_data.size() - size; }
    std::optional<quint32> u32()
    {
        if (!fits(4))
            return std::nullopt;
        const quint32 v = qFromLittleEndian<quint32>(m_data.constData() + m_at);
        m_at += 4;
        return v;
    }
    bool skip(qint64 size)
    {
        if (!fits(size))
            return false;
        m_at += size;
        return true;
    }
    qint64 at() const { return m_at; }
    void seek(qint64 at) { m_at = at; }
    const char *here() const { return m_data.constData() + m_at; }

private:
    const QByteArray &m_data;
    qint64 m_at = 0;
};

bool plausibleLanguageHeader(QByteArrayView head)
{
    if (head.size() < 8)
        return false;
    const quint32 count = qFromLittleEndian<quint32>(head.data());
    if (count == 0 || count > quint32(kMaxLanguages))
        return false;
    const qint64 identifiersAt = 4 + 4 * qint64(count);
    quint32 previous = 0;
    for (quint32 i = 0; i < count && 8 + 4 * qint64(i) <= head.size(); ++i) {
        const quint32 position = qFromLittleEndian<quint32>(head.data() + 4 + 4 * i);
        if (position < identifiersAt + 8 || position < previous)
            return false;
        previous = position;
    }
    if (identifiersAt + 4 > head.size())
        return true;
    const quint32 version = qFromLittleEndian<quint32>(head.data() + identifiersAt);
    return version >= 256 && version <= 4096;
}

std::optional<LanguageTable> parseLanguageTable(const QByteArray &data, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<LanguageTable> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (!plausibleLanguageHeader(data))
        return fail(QObject::tr("Not a RAGE string table (.strtbl)."));
    Cursor c(data);
    const quint32 count = *c.u32();
    std::vector<quint32> positions;
    for (quint32 i = 0; i < count; ++i) {
        const std::optional<quint32> p = c.u32();
        if (!p || *p > data.size())
            return fail(QObject::tr("The string table is damaged."));
        positions.push_back(*p);
    }
    const std::optional<quint32> version = c.u32(), identifierCount = c.u32();
    if (!version || *version < 256 || *version > 4096 || !identifierCount || *identifierCount > kMaxStrings)
        return fail(QObject::tr("The string table is damaged."));
    LanguageTable table;
    for (quint32 i = 0; i < *identifierCount; ++i) {
        const std::optional<quint32> length = c.u32();
        if (!length || *length > quint32(kMaxStringBytes) || !c.fits(qint64(*length) + 1) || c.here()[*length] != '\0')
            return fail(QObject::tr("The string table is damaged."));
        table.identifiers << QString::fromLatin1(c.here(), *length).replace(QChar(0xA0), u'?');
        c.skip(*length + 1);
    }
    const qint64 identifiersEnd = c.at();
    QSet<quint32> seen;
    for (quint32 i = 0; i < count; ++i) {
        const quint32 position = positions[i];
        if (position == data.size())
            continue;
        if (seen.contains(position))
            continue;
        if (position < identifiersEnd)
            return fail(QObject::tr("The string table is damaged."));
        seen.insert(position);
        c.seek(position);
        const std::optional<quint32> strings = c.u32();
        if (!strings || *strings > kMaxStrings)
            return fail(QObject::tr("The string table is damaged."));
        LanguageBlock block;
        block.index = int(i);
        for (quint32 k = 0; k < *strings; ++k) {
            const std::optional<quint32> hash = c.u32();
            if (!hash || !c.skip(2))
                return fail(QObject::tr("The string table is damaged."));
            const std::optional<quint32> noteLength = c.u32();
            if (!noteLength || *noteLength > quint32(kMaxStringBytes) || !c.fits(*noteLength))
                return fail(QObject::tr("The string table is damaged."));
            if (*noteLength > 0)
                block.notes.insert(*hash, QString::fromLatin1(c.here(), *noteLength));
            c.skip(*noteLength);
            const std::optional<quint32> length = c.u32();
            if (!length || *length > quint32(kMaxStringBytes) || !c.fits(qint64(*length) * 2 + 10))
                return fail(QObject::tr("The string table is damaged."));
            QString text(reinterpret_cast<const QChar *>(c.here()), *length);
            for (QChar &ch : text)
                ch = QChar(qFromLittleEndian<quint16>(&ch));
            while (text.endsWith(QChar(u'\0')))
                text.chop(1);
            c.skip(qint64(*length) * 2 + 10);
            if (!block.texts.contains(*hash))
                block.order.push_back(*hash);
            block.texts.insert(*hash, std::move(text));
        }
        table.languages.push_back(std::move(block));
    }
    return table;
}

}

int RageStringTableReader::score(const QString &fileName, QByteArrayView head) const
{
    if (!isRageResource(head) || qFromLittleEndian<quint32>(head.data() + 4) != kType)
        return 0;
    const qsizetype bodyAt = uchar(head[3]) == 0x85 ? 16 : 12;
    if (!looksLikeStringTable(head.sliced(std::min(bodyAt, head.size()))))
        return 0;
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    return suffix == QLatin1String("sst") || suffix == QLatin1String("wst") ? 100 : 30;
}

std::unique_ptr<Document> RageStringTableReader::read(const QString &path, QString *error) const
{
    std::optional<QByteArray> data = readWholeFile(path, kMaxFileSize, error);
    return data ? readBytes(*data, path, error) : nullptr;
}

std::unique_ptr<Document> RageStringTableReader::readBytes(const QByteArray &data, const QString &fileName, QString *error) const
{
    std::optional<std::vector<HashedString>> strings = parseResource(data, error);
    if (!strings)
        return nullptr;
    std::vector<SimpleTable::Row> rows;
    rows.reserve(strings->size());
    for (const HashedString &s : *strings)
        rows.push_back({hashName(s.hash), shown(s.text)});
    const QString base = QFileInfo(fileName).completeBaseName();
    auto table = std::make_shared<SimpleTable>(base.isEmpty() ? QObject::tr("Strings") : base,
                                               std::vector<SimpleColumn>{{QObject::tr("hash"), false, QObject::tr("Hash of the string's name")},
                                                                         {QObject::tr("text")}},
                                               std::move(rows));
    QString description = QObject::tr("RAGE string table, %n string(s)", nullptr, int(strings->size()));
    if (const QString language = languageFromName(fileName); !language.isEmpty())
        description += QStringLiteral(", ") + language;
    table->setDescription(description);
    table->setJsonExporter([strings = std::move(*strings)] {
        QJsonObject out;
        for (const HashedString &s : strings)
            out.insert(hashName(s.hash), s.text);
        return QJsonDocument(out).toJson();
    });
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(table));
    return doc;
}

int RageLanguageTableReader::score(const QString &fileName, QByteArrayView head) const
{
    if (!plausibleLanguageHeader(head))
        return 0;
    return QFileInfo(fileName).suffix().compare(QLatin1String("strtbl"), Qt::CaseInsensitive) == 0 ? 100 : 10;
}

std::unique_ptr<Document> RageLanguageTableReader::read(const QString &path, QString *error) const
{
    std::optional<QByteArray> data = readWholeFile(path, kMaxFileSize, error);
    return data ? readBytes(*data, path, error) : nullptr;
}

std::unique_ptr<Document> RageLanguageTableReader::readBytes(const QByteArray &data, const QString &fileName, QString *error) const
{
    std::optional<LanguageTable> parsed = parseLanguageTable(data, error);
    if (!parsed)
        return nullptr;
    auto table = std::make_shared<const LanguageTable>(std::move(*parsed));

    std::vector<quint32> hashes;
    QStringList names;
    QSet<quint32> known;
    for (const QString &id : table->identifiers) {
        const quint32 h = joaat(id.toLatin1());
        if (known.contains(h))
            continue;
        known.insert(h);
        hashes.push_back(h);
        names << id;
    }
    for (const LanguageBlock &block : table->languages)
        for (quint32 h : block.order)
            if (!known.contains(h)) {
                known.insert(h);
                hashes.push_back(h);
                names << hashName(h);
            }

    const bool hasNotes = std::any_of(table->languages.begin(), table->languages.end(),
                                      [](const LanguageBlock &b) { return !b.notes.isEmpty(); });
    std::vector<SimpleColumn> columns{{QObject::tr("name")}};
    if (hasNotes)
        columns.push_back({QObject::tr("note"), false, QObject::tr("A label stored next to the string")});
    for (const LanguageBlock &block : table->languages)
        columns.push_back({languageAt(block.index)});
    std::vector<SimpleTable::Row> rows;
    rows.reserve(hashes.size());
    for (size_t i = 0; i < hashes.size(); ++i) {
        SimpleTable::Row row{names[qsizetype(i)]};
        if (hasNotes) {
            QString note;
            for (const LanguageBlock &block : table->languages)
                if (note.isEmpty())
                    note = block.notes.value(hashes[i]);
            row.push_back(note);
        }
        for (const LanguageBlock &block : table->languages)
            row.push_back(shown(block.texts.value(hashes[i])));
        rows.push_back(std::move(row));
    }
    const QString base = QFileInfo(fileName).completeBaseName();
    auto out = std::make_shared<SimpleTable>(base.isEmpty() ? QObject::tr("Strings") : base, std::move(columns), std::move(rows));
    QString description = QObject::tr("RAGE string table, %n string(s)", nullptr, int(hashes.size()))
                          + QObject::tr(" in %n language(s)", nullptr, int(table->languages.size()));
    out->setDescription(description);
    out->setJsonExporter([table, hashes, names] {
        QJsonObject result;
        for (const LanguageBlock &block : table->languages) {
            QJsonObject language;
            for (size_t i = 0; i < hashes.size(); ++i)
                if (const auto it = block.texts.constFind(hashes[i]); it != block.texts.cend())
                    language.insert(names[qsizetype(i)], *it);
            result.insert(languageAt(block.index), language);
        }
        return QJsonDocument(result).toJson();
    });
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(out));
    return doc;
}
