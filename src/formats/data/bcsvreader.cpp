#include "data/bcsvreader.h"

#include "core/records.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QObject>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <mutex>

namespace {

constexpr qsizetype kBaseHeader = 0x0C;
constexpr qsizetype kExtendedHeader = 0x10;
constexpr int kTextSamples = 64;

const QStringList &typeSuffixes()
{
    static const QStringList suffixes = {"u8", "s8", "u16", "s16", "u32", "s32", "f32", "u64", "s64", "string", "str"};
    return suffixes;
}

std::mutex &namesMutex()
{
    static std::mutex m;
    return m;
}

std::shared_ptr<const QHash<quint32, HashedName>> &namesTable()
{
    static std::shared_ptr<const QHash<quint32, HashedName>> table = std::make_shared<QHash<quint32, HashedName>>();
    return table;
}

std::optional<FieldType> typeFromSuffix(const QString &suffix)
{
    if (suffix == "u8")
        return FieldType::U8;
    if (suffix == "s8")
        return FieldType::I8;
    if (suffix == "u16")
        return FieldType::U16;
    if (suffix == "s16")
        return FieldType::I16;
    if (suffix == "u32")
        return FieldType::U32;
    if (suffix == "s32")
        return FieldType::I32;
    if (suffix == "f32")
        return FieldType::F32;
    if (suffix == "u64" || suffix == "s64")
        return FieldType::U64;
    return std::nullopt;
}

struct Field
{
    quint32 hash = 0;
    int offset = 0;
};

struct Layout
{
    int count = 0;
    int size = 0;
    qsizetype dataStart = 0;
    bool extended = false;
    std::vector<Field> fields;
};

std::optional<Layout> readLayout(QByteArrayView data, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<Layout> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (data.size() < kBaseHeader)
        return fail(QObject::tr("The file is too short for a BCSV table."));
    Layout l;
    const quint32 count = qFromLittleEndian<quint32>(data.data());
    const quint32 size = qFromLittleEndian<quint32>(data.data() + 4);
    const quint16 fieldCount = qFromLittleEndian<quint16>(data.data() + 8);
    l.extended = data[10] != 0;
    const qsizetype fieldTable = kBaseHeader + (l.extended ? kExtendedHeader : 0);
    if (size == 0 || size > 0x100000 || fieldCount == 0)
        return fail(QObject::tr("Not a BCSV table: unexpected record size or field count."));
    l.dataStart = fieldTable + qsizetype(fieldCount) * 8;
    if (l.dataStart > data.size() || qint64(count) * size > data.size() - l.dataStart)
        return fail(QObject::tr("Not a BCSV table: the records do not fit in the file."));
    l.count = int(count);
    l.size = int(size);
    for (int i = 0; i < fieldCount; ++i) {
        const char *p = data.data() + fieldTable + qsizetype(i) * 8;
        Field f{qFromLittleEndian<quint32>(p), int(qFromLittleEndian<quint32>(p + 4))};
        if (f.offset < 0 || f.offset >= l.size)
            return fail(QObject::tr("Not a BCSV table: a field starts outside the record."));
        l.fields.push_back(f);
    }
    std::sort(l.fields.begin(), l.fields.end(), [](const Field &a, const Field &b) { return a.offset < b.offset; });
    return l;
}

bool looksLikeText(const QByteArray &data, const Layout &l, int offset, int width)
{
    if (width < 4)
        return false;
    bool anyText = false;
    const int samples = std::min(l.count, kTextSamples);
    for (int r = 0; r < samples; ++r) {
        const char *p = data.constData() + l.dataStart + qsizetype(r) * l.size + offset;
        const int length = int(qstrnlen(p, uint(width)));
        for (int i = length; i < width; ++i)
            if (p[i] != 0)
                return false;
        for (int i = 0; i < length; ++i)
            if (uchar(p[i]) < 0x20)
                return false;
        if (length > 0) {
            if (!QString::fromUtf8(p, length).isValidUtf16() || QString::fromUtf8(p, length).contains(QChar::ReplacementCharacter))
                return false;
            anyText = true;
        }
    }
    return anyText || (samples > 0 && width >= 16);
}

class BcsvRecords : public RecordSource
{
public:
    BcsvRecords(QByteArray data, Layout layout, QString name)
        : m_data(std::move(data)), m_layout(std::move(layout)), m_name(std::move(name))
    {
    }

    QString recordTypeName() const override { return m_name; }
    int recordSize() const override { return m_layout.size; }
    int recordCount() const override { return m_layout.count; }
    QByteArrayView record(int row) const override
    {
        return QByteArrayView(m_data).sliced(m_layout.dataStart + qsizetype(row) * m_layout.size, m_layout.size);
    }

    std::vector<FieldDef> guessFields() const override
    {
        std::vector<FieldDef> out;
        const auto &fields = m_layout.fields;
        if (!fields.empty() && fields.front().offset >= 4) {
            FieldDef first{0, FieldType::Auto32, {}, false};
            if (m_layout.count > 0 && qFromLittleEndian<quint32>(record(0).data()) == quint32(m_layout.dataStart))
                first = {0, FieldType::U32, QObject::tr("(record offset)"), false};
            out.push_back(first);
        }
        for (size_t i = 0; i < fields.size(); ++i) {
            const Field &f = fields[i];
            const int next = i + 1 < fields.size() ? fields[i + 1].offset : m_layout.size;
            const int width = next - f.offset;
            if (width <= 0)
                continue;
            FieldDef def;
            def.offset = f.offset;
            const std::optional<HashedName> known = BcsvReader::nameForHash(f.hash);
            def.name = known ? known->name : QStringLiteral("0x") + QString::number(f.hash, 16).toUpper().rightJustified(8, u'0');
            std::optional<FieldType> type = known ? typeFromSuffix(known->type) : std::nullopt;
            if (type && fieldSize(*type) > width)
                type.reset();
            if (!type) {
                const bool namedString = known && (known->type == "string" || known->type == "str");
                if (namedString
                    || (width != 1 && width != 2 && width != 4 && looksLikeText(m_data, m_layout, f.offset, width)))
                    type = FieldType::Chars;
                else if (width == 1)
                    type = FieldType::U8;
                else if (width == 2)
                    type = FieldType::U16;
                else if (width == 8)
                    type = FieldType::U64;
                else
                    type = FieldType::Auto32;
            }
            def.type = *type;
            if (def.type == FieldType::Chars)
                def.size = width;
            if (def.end() <= m_layout.size)
                out.push_back(def);
        }
        return out;
    }

    QString description() const override
    {
        int named = 0;
        for (const Field &f : m_layout.fields)
            named += BcsvReader::nameForHash(f.hash) ? 1 : 0;
        return QObject::tr("— %1 of %2 column names known").arg(named).arg(m_layout.fields.size());
    }

private:
    QByteArray m_data;
    Layout m_layout;
    QString m_name;
};

}

quint32 BcsvReader::crc32(QByteArrayView data)
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> t{};
        for (quint32 n = 0; n < 256; ++n) {
            quint32 c = n;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    quint32 c = 0xFFFFFFFFu;
    for (char byte : data)
        c = table[(c ^ uchar(byte)) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void BcsvReader::setHashNames(const QStringList &lines)
{
    auto table = std::make_shared<QHash<quint32, HashedName>>();
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(u'#'))
            continue;
        const qsizetype space = line.lastIndexOf(u' ');
        const QString suffix = space > 0 ? line.mid(space + 1) : QString();
        if (space > 0 && typeSuffixes().contains(suffix)) {
            table->insert(crc32(line.toUtf8()), {line.left(space), suffix});
            continue;
        }
        table->insert(crc32(line.toUtf8()), {line, QString()});
        for (const QString &s : typeSuffixes())
            table->insert(crc32((line + u' ' + s).toUtf8()), {line, s});
    }
    const std::lock_guard lock(namesMutex());
    namesTable() = std::move(table);
}

std::optional<HashedName> BcsvReader::nameForHash(quint32 hash)
{
    std::shared_ptr<const QHash<quint32, HashedName>> table;
    {
        const std::lock_guard lock(namesMutex());
        table = namesTable();
    }
    const auto it = table->find(hash);
    if (it == table->end())
        return std::nullopt;
    return *it;
}

int BcsvReader::score(const QString &fileName, QByteArrayView head) const
{
    const bool named = fileName.endsWith(QLatin1String(".bcsv"), Qt::CaseInsensitive);
    const bool signed_ = head.size() >= 0x10 && head.sliced(0x0C, 4) == "VSCB";
    if (signed_)
        return named ? 95 : 80;
    return named ? 70 : 0;
}

std::unique_ptr<Document> BcsvReader::read(const QString &path, QString *error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return nullptr;
    }
    if (file.size() > kMaxFileSize) {
        if (error)
            *error = QObject::tr("File is larger than 256 MB.");
        return nullptr;
    }
    return readBytes(file.readAll(), path, error);
}

std::unique_ptr<Document> BcsvReader::readBytes(const QByteArray &data, const QString &fileName, QString *error) const
{
    std::optional<Layout> layout = readLayout(data, error);
    if (!layout)
        return nullptr;
    QString name = QFileInfo(fileName).completeBaseName();
    if (name.isEmpty())
        name = QStringLiteral("BCSV");
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::make_shared<RecordTable>(std::make_shared<BcsvRecords>(data, std::move(*layout), name)));
    return doc;
}
