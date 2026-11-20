#include "data/falcomscriptreader.h"
#include "core/bytes.h"
#include "core/storedreader.h"
#include "data/ed9bytecode.h"
#include "core/simpletable.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QtEndian>

#include <algorithm>
#include <optional>
#include <set>

namespace {

constexpr qsizetype kHeaderSize = 0x18;
constexpr qsizetype kFunctionEntrySize = 32;
constexpr quint32 kMaxFunctions = 100000;
constexpr quint32 kTagString = 3;
constexpr uchar kOpCall = 0x0C;

struct Function
{
    QString name;
    quint32 start = 0, size = 0, hash = 0, nameOffset = 0;
    quint8 inputs = 0, outputs = 0;
    quint16 flags = 0;
};

class CodeTable : public Table
{
public:
    struct Row
    {
        quint32 offset;
        quint32 function;
    };

    CodeTable(QByteArray data, std::vector<Row> rows, QStringList functionNames, QStringList calleeNames,
              QString description)
        : m_data(std::move(data)), m_rows(std::move(rows)), m_functionNames(std::move(functionNames)),
          m_calleeNames(std::move(calleeNames)), m_description(std::move(description)) {}

    QString name() const override { return QObject::tr("Code"); }
    int rowCount() const override { return int(m_rows.size()); }
    int columnCount() const override { return 4; }
    QString columnName(int column) const override
    {
        static const char *const names[] = {"function", "offset", "instruction", "arguments"};
        return QObject::tr(names[column]);
    }
    QString text(int row, int column, DisplayMode) const override
    {
        const Row &r = m_rows.at(size_t(row));
        switch (column) {
        case 0: return m_functionNames.at(r.function);
        case 1: return QStringLiteral("0x") + QString::number(r.offset, 16).toUpper();
        case 2: return ed9::decode(m_data, r.offset).name;
        default: {
            if (uchar(m_data[r.offset]) == kOpCall) {
                const int callee = qFromLittleEndian<quint16>(m_data.constData() + r.offset + 1);
                if (callee < m_calleeNames.size())
                    return QStringLiteral("%1  (function %2)").arg(m_calleeNames.at(callee)).arg(callee);
            }
            return ed9::decode(m_data, r.offset).arguments;
        }
        }
    }
    QVariant sortKey(int row, int column) const override
    {
        if (column == 1)
            return m_rows.at(size_t(row)).offset;
        return text(row, column, DisplayMode::Auto);
    }
    bool isNumeric(int column) const override { return column == 1; }
    bool monospace() const override { return true; }
    QString description() const override { return m_description; }

private:
    QByteArray m_data;
    std::vector<Row> m_rows;
    QStringList m_functionNames;
    QStringList m_calleeNames;
    QString m_description;
};

struct PoolString
{
    quint32 offset = 0;
    QString text;
};


std::optional<quint32> stringOffset(const QByteArray &data, quint32 tagged)
{
    if (tagged >> 30 != kTagString)
        return std::nullopt;
    const quint32 offset = tagged & 0x3FFFFFFF;
    if (offset >= quint32(data.size()) || data.indexOf('\0', offset) < 0)
        return std::nullopt;
    return offset;
}

QString displayText(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (QChar c : text) {
        if (c == u'\n')
            out += QStringLiteral(" ↵ ");
        else if (c.unicode() < 0x20)
            out += QStringLiteral("\\x%1").arg(uint(c.unicode()), 2, 16, QLatin1Char('0'));
        else
            out += c;
    }
    return out;
}

bool isTextByte(char c)
{
    return uchar(c) >= 0x20 || c == '\0' || c == '\t' || c == '\n';
}

QByteArray toJson(const QJsonArray &array) { return QJsonDocument(array).toJson(QJsonDocument::Indented); }

}

int FalcomScriptReader::score(const QString &, QByteArrayView head) const
{
    return head.startsWith("#scp") ? 100 : 0;
}

std::unique_ptr<Document> FalcomScriptReader::read(const QString &path, QString *error) const
{
    const std::optional<QByteArray> bytes = readWholeFile(path, kMaxFileSize, error);
    if (!bytes)
        return nullptr;
    return fromBytes(*bytes, error);
}

std::unique_ptr<Document> FalcomScriptReader::fromBytes(const QByteArray &data, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return nullptr;
    };
    if (data.size() < kHeaderSize || !data.startsWith("#scp"))
        return fail(QObject::tr("Not a Falcom script (no \"#scp\" at the start)."));

    const quint32 tableOffset = le32(data, 4), count = le32(data, 8);
    if (tableOffset < kHeaderSize || count > kMaxFunctions
        || qint64(tableOffset) + qint64(count) * kFunctionEntrySize > data.size())
        return fail(QObject::tr("The function table does not fit in the file."));
    const qsizetype tableEnd = tableOffset + qsizetype(count) * kFunctionEntrySize;

    std::vector<Function> functions(count);
    for (quint32 i = 0; i < count; ++i) {
        const qsizetype e = tableOffset + qsizetype(i) * kFunctionEntrySize;
        Function &f = functions[i];
        f.start = le32(data, e);
        f.inputs = quint8(data[e + 4]);
        f.flags = quint16(uchar(data[e + 5]) | uchar(data[e + 6]) << 8);
        f.outputs = quint8(data[e + 7]);
        f.hash = le32(data, e + 24);
        const std::optional<quint32> nameOffset = stringOffset(data, le32(data, e + 28));
        if (!nameOffset || f.start > quint32(data.size()))
            return fail(QObject::tr("Function %1 in the table is broken.").arg(i));
        f.nameOffset = *nameOffset;
        f.name = QString::fromUtf8(data.constData() + f.nameOffset);
    }

    quint32 poolStart = quint32(data.size());
    quint32 lastStart = quint32(tableEnd);
    for (const Function &f : functions) {
        poolStart = std::min(poolStart, f.nameOffset);
        lastStart = std::max(lastStart, f.start);
    }
    std::vector<quint32> references;
    for (qsizetype i = tableEnd; i + 4 <= data.size(); ++i) {
        const quint32 v = le32(data, i);
        if (v >> 30 != kTagString)
            continue;
        const quint32 offset = v & 0x3FFFFFFF;
        if (offset > lastStart && offset < poolStart) {
            references.push_back(offset);
            if (data[offset - 1] == '\0')
                poolStart = offset;
        }
    }

    std::sort(references.begin(), references.end());
    qsizetype cleanFrom = poolStart;
    while (cleanFrom > tableEnd && isTextByte(data[cleanFrom - 1]))
        --cleanFrom;
    for (const quint32 offset : references) {
        if (offset >= poolStart)
            break;
        if (offset >= cleanFrom && QUtf8StringView(QByteArrayView(data).sliced(offset, poolStart - offset)).isValidUtf8()) {
            poolStart = offset;
            break;
        }
    }

    std::vector<PoolString> strings;
    for (qsizetype pos = poolStart; pos < data.size();) {
        qsizetype end = data.indexOf('\0', pos);
        if (end < 0)
            end = data.size();
        strings.push_back({quint32(pos), QString::fromUtf8(data.constData() + pos, end - pos)});
        pos = end + 1;
    }

    std::set<quint32> starts;
    for (const Function &f : functions)
        starts.insert(f.start);
    for (Function &f : functions) {
        const auto next = starts.upper_bound(f.start);
        const quint32 end = next != starts.end() ? *next : poolStart;
        f.size = end > f.start ? end - f.start : 0;
    }

    std::set<quint32> nameOffsets;
    for (const Function &f : functions)
        nameOffsets.insert(f.nameOffset);

    std::vector<SimpleTable::Row> stringRows;
    stringRows.reserve(strings.size());
    for (const PoolString &s : strings) {
        stringRows.push_back({s.offset, displayText(s.text),
                              nameOffsets.count(s.offset) ? QObject::tr("function name") : QString()});
    }
    auto stringTable = std::make_shared<SimpleTable>(
        QObject::tr("Strings"),
        std::vector<SimpleColumn>{{QObject::tr("offset"), true},
                                  {QObject::tr("text"), false, QObject::tr("Line breaks are shown as ↵")},
                                  {QObject::tr("used as")}},
        std::move(stringRows));
    stringTable->setDescription(QObject::tr("%1 strings, from 0x%2").arg(strings.size()).arg(poolStart, 0, 16));
    stringTable->setJsonExporter([strings] {
        QJsonArray array;
        for (const PoolString &s : strings)
            array.append(QJsonObject{{"offset", qint64(s.offset)}, {"text", s.text}});
        return toJson(array);
    });

    std::vector<SimpleTable::Row> functionRows;
    functionRows.reserve(functions.size());
    for (const Function &f : functions)
        functionRows.push_back({f.name, f.start, f.size, int(f.inputs), int(f.outputs), int(f.flags), f.hash});
    auto functionTable = std::make_shared<SimpleTable>(
        QObject::tr("Functions"),
        std::vector<SimpleColumn>{{QObject::tr("name")},
                                  {QObject::tr("start"), true, QObject::tr("Where its bytecode starts in the file")},
                                  {QObject::tr("size"), false, QObject::tr("Bytes of bytecode")},
                                  {QObject::tr("inputs"), false, QObject::tr("Number of arguments it takes")},
                                  {QObject::tr("outputs"), false, QObject::tr("Number of values it returns")},
                                  {QObject::tr("flags"), true},
                                  {QObject::tr("hash"), true}},
        std::move(functionRows));
    functionTable->setDescription(QObject::tr("%1 functions").arg(functions.size()));
    functionTable->setJsonExporter([functions] {
        QJsonArray array;
        for (const Function &f : functions) {
            array.append(QJsonObject{{"name", f.name}, {"start", qint64(f.start)}, {"size", qint64(f.size)},
                                     {"inputs", f.inputs}, {"outputs", f.outputs}, {"flags", f.flags},
                                     {"hash", qint64(f.hash)}});
        }
        return toJson(array);
    });

    std::vector<size_t> byStart(functions.size());
    for (size_t i = 0; i < byStart.size(); ++i)
        byStart[i] = i;
    std::sort(byStart.begin(), byStart.end(), [&](size_t a, size_t b) { return functions[a].start < functions[b].start; });
    std::vector<CodeTable::Row> code;
    QStringList functionNames;
    int undecoded = 0;
    for (size_t i : byStart) {
        const Function &f = functions[i];
        functionNames << f.name;
        const qsizetype end = qsizetype(f.start) + f.size;
        for (qsizetype pos = f.start; pos < end;) {
            const int size = ed9::instructionSize(data, pos);
            if (size <= 0 || pos + size > end) {
                ++undecoded; 
                break;
            }
            code.push_back({quint32(pos), quint32(functionNames.size() - 1)});
            pos += size;
        }
    }
    QString codeDescription = QObject::tr("%1 instructions").arg(code.size());
    if (undecoded > 0)
        codeDescription += QObject::tr(", %1 functions not fully decoded").arg(undecoded);
    QStringList calleeNames;
    for (const Function &f : functions)
        calleeNames << f.name;
    auto codeTable = std::make_shared<CodeTable>(data, std::move(code), functionNames, calleeNames, codeDescription);

    auto doc = std::make_unique<Document>();
    doc->tables.push_back(stringTable);
    doc->tables.push_back(functionTable);
    doc->tables.push_back(codeTable);
    return doc;
}
