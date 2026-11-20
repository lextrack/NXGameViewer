#include "data/textreader.h"
#include "core/simpletable.h"
#include "core/storedreader.h"

#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QRegularExpression>

#include <algorithm>

namespace {

const QStringList &textSuffixes()
{
    static const QStringList suffixes = {
        "txt", "csv", "tsv", "xml", "ini", "cfg", "conf", "bat", "cmd", "sh", "lua", "log", "md", "html", "htm",
        "yaml", "yml", "py", "js", "css", "plist", "properties", "srt", "toml", "tab",
    };
    return suffixes;
}

bool isTable(const QString &fileName, QChar *separator)
{
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    if (suffix == QLatin1String("csv")) {
        *separator = u',';
        return true;
    }
    if (suffix == QLatin1String("tsv") || suffix == QLatin1String("tab")) {
        *separator = u'\t';
        return true;
    }
    return false;
}

QChar guessCsvSeparator(QStringView text)
{
    const QStringView firstLines = text.first(std::min<qsizetype>(text.size(), 4096));
    const qsizetype commas = firstLines.count(u','), semicolons = firstLines.count(u';'), tabs = firstLines.count(u'\t');
    if (tabs > commas && tabs > semicolons)
        return u'\t';
    return semicolons > commas ? u';' : u',';
}

std::vector<QStringList> parseCsv(QStringView text, QChar separator)
{
    std::vector<QStringList> rows;
    QStringList row;
    QString field;
    bool quoted = false, fieldStarted = false;
    auto endField = [&] {
        row << field;
        field.clear();
        fieldStarted = false;
    };
    auto endRow = [&] {
        endField();
        rows.push_back(std::move(row));
        row = {};
    };
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (quoted) {
            if (c == u'"') {
                if (i + 1 < text.size() && text[i + 1] == u'"') {
                    field += u'"';
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field += c;
            }
        } else if (c == u'"' && !fieldStarted) {
            quoted = true;
            fieldStarted = true;
        } else if (c == separator) {
            endField();
        } else if (c == u'\r' || c == u'\n') {
            if (c == u'\r' && i + 1 < text.size() && text[i + 1] == u'\n')
                ++i;
            endRow();
        } else {
            field += c;
            fieldStarted = true;
        }
    }
    if (fieldStarted || !row.isEmpty() || !field.isEmpty())
        endRow();
    return rows;
}

QVariant cellValue(const QString &field)
{
    if (field.isEmpty() || field.size() > 18 || (field.size() > 1 && field.startsWith(u'0')))
        return field;
    bool ok = false;
    const qint64 n = field.toLongLong(&ok);
    return ok && !field.startsWith(u'+') && !field.contains(u' ') ? QVariant(n) : QVariant(field);
}

QString visible(QString line)
{
    for (QChar &c : line) {
        if (c.unicode() < 0x20 && c != u'\t')
            c = QChar(0x2400 + c.unicode());
    }
    return line;
}

class TextTable : public SimpleTable
{
public:
    TextTable(QString name, std::vector<SimpleColumn> columns, std::vector<Row> rows, QByteArray raw,
              QString fileName, bool lineNumbers)
        : SimpleTable(std::move(name), std::move(columns), std::move(rows)), m_raw(std::move(raw)),
          m_fileName(std::move(fileName)), m_lineNumbers(lineNumbers) {}

    QString rowLabel(int row) const override
    {
        return m_lineNumbers ? QString::number(row + 1) : SimpleTable::rowLabel(row);
    }

    bool canChangeTextEncoding() const override { return true; }

    std::shared_ptr<const Table> withTextEncoding(TextEncoding encoding) const override
    {
        const auto doc = TextReader::fromBytes(m_raw, m_fileName, nullptr, encoding, false);
        return doc ? doc->tables.front() : nullptr;
    }

private:
    QByteArray m_raw;
    QString m_fileName;
    bool m_lineNumbers;
};

}

int TextReader::score(const QString &fileName, QByteArrayView head) const
{
    if (textSuffixes().contains(QFileInfo(fileName).suffix().toLower()))
        return 40;
    return looksLikeText(head) ? 15 : 0;
}

std::unique_ptr<Document> TextReader::read(const QString &path, QString *error) const
{
    const std::optional<QByteArray> bytes = readWholeFile(path, kMaxFileSize, error);
    if (!bytes)
        return nullptr;
    return fromBytes(*bytes, QFileInfo(path).fileName(), error);
}

std::unique_ptr<Document> TextReader::fromBytes(const QByteArray &data, const QString &fileName, QString *error,
                                                std::optional<TextEncoding> encoding, bool requireText)
{
    DetectedEncoding detected = detectTextEncoding(data);
    if (encoding && *encoding != detected.encoding) {
        detected.encoding = *encoding;
        detected.bomSize = 0;
        detected.confident = false;
    }
    QByteArrayView body = QByteArrayView(data).sliced(detected.bomSize);
    if (detected.encoding == TextEncoding::Utf8 && body.startsWith("\xEF\xBB\xBF"))
        body = body.sliced(3);
    else if (detected.encoding == TextEncoding::Utf16LE && body.startsWith("\xFF\xFE"))
        body = body.sliced(2);
    else if (detected.encoding == TextEncoding::Utf16BE && body.startsWith("\xFE\xFF"))
        body = body.sliced(2);

    const QString text = decodeText(body, detected.encoding);
    if (requireText) {
        qsizetype bad = 0;
        for (QChar c : text) {
            if ((c.unicode() < 0x20 && c != u'\t' && c != u'\n' && c != u'\r' && c != u'\f')
                || c == QChar::ReplacementCharacter)
                ++bad;
        }
        if (bad * 50 > text.size()) {
            if (error)
                *error = QObject::tr("This does not look like text.");
            return nullptr;
        }
    }

    QString encodingInfo = encodingName(detected.encoding);
    if (detected.bomSize > 0)
        encodingInfo += QObject::tr(" with BOM");

    std::shared_ptr<TextTable> table;
    QChar separator;
    if (isTable(fileName, &separator)) {
        if (separator == u',')
            separator = guessCsvSeparator(text);
        std::vector<QStringList> fields = parseCsv(text, separator);
        size_t columnCount = 0;
        for (const QStringList &row : fields)
            columnCount = std::max(columnCount, size_t(row.size()));
        std::vector<SimpleColumn> columns;
        for (size_t c = 0; c < columnCount; ++c)
            columns.push_back({QObject::tr("col %1").arg(c + 1)});
        std::vector<SimpleTable::Row> rows;
        rows.reserve(fields.size());
        for (const QStringList &row : fields) {
            SimpleTable::Row cells;
            cells.reserve(size_t(row.size()));
            for (const QString &field : row)
                cells.push_back(cellValue(visible(field)));
            rows.push_back(std::move(cells));
        }
        const size_t rowCount = rows.size();
        table = std::make_shared<TextTable>(QObject::tr("Table"), std::move(columns), std::move(rows), data, fileName,
                                            true);
        table->setDescription(QObject::tr("%1 rows × %2 columns, %3").arg(rowCount).arg(columnCount).arg(encodingInfo));
    } else {
        std::vector<SimpleTable::Row> rows;
        const QStringList lines = text.split(QRegularExpression(QStringLiteral("\r\n|\n|\r")));
        rows.reserve(size_t(lines.size()));
        for (const QString &line : lines)
            rows.push_back({visible(line)});
        if (rows.size() > 1 && lines.back().isEmpty())
            rows.pop_back();
        const size_t rowCount = rows.size();
        table = std::make_shared<TextTable>(QObject::tr("Text"), std::vector<SimpleColumn>{{QObject::tr("text")}},
                                            std::move(rows), data, fileName, true);
        table->setDescription(QObject::tr("%1 lines, %2").arg(rowCount).arg(encodingInfo));
    }

    auto doc = std::make_unique<Document>();
    doc->tables.push_back(table);
    return doc;
}
