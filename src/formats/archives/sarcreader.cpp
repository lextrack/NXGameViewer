#include "archives/sarcreader.h"

#include "archives/sarcwriter.h"
#include "core/contentkind.h"
#include "core/simpletable.h"
#include "core/storedreader.h"

#include <QObject>

namespace {

class ArchiveTable : public SimpleTable
{
public:
    ArchiveTable(std::vector<Row> rows, std::shared_ptr<const Archive> archive)
        : SimpleTable(QObject::tr("Files"),
                      {{QObject::tr("name")}, {QObject::tr("size")}, {QObject::tr("type")}}, std::move(rows)),
          m_archive(std::move(archive))
    {
    }

    std::shared_ptr<const Archive> archive() const override { return m_archive; }

private:
    std::shared_ptr<const Archive> m_archive;
};

}

int SarcReader::score(const QString &, QByteArrayView head) const
{
    return head.startsWith("SARC") ? 100 : 0;
}

std::unique_ptr<Document> SarcReader::read(const QString &path, QString *error) const
{
    const std::optional<QByteArray> bytes = readWholeFile(path, kMaxFileSize, error);
    if (!bytes)
        return nullptr;
    return readBytes(*bytes, path, error);
}

std::unique_ptr<Document> SarcReader::readBytes(const QByteArray &data, const QString &, QString *error) const
{
    const std::optional<SarcIndex> index = parseSarcIndex(data, error);
    if (!index)
        return nullptr;

    auto archive = std::make_shared<Archive>();
    archive->data = data;
    std::vector<SimpleTable::Row> rows;
    QStringList problems;
    for (const SarcNode &node : index->nodes) {
        ArchiveEntry entry;
        entry.name = sarcNodeName(node);
        if (!node.inside) {
            problems << entry.name;
            continue;
        }
        entry.offset = node.start;
        entry.size = node.end - node.start;
        rows.push_back({entry.name, qint64(entry.size),
                        describeContent(QByteArrayView(data).sliced(node.start, std::min<qsizetype>(entry.size, 16)))});
        archive->entries.push_back(std::move(entry));
    }

    auto table = std::make_shared<ArchiveTable>(std::move(rows), archive);
    QString description = QObject::tr("%n file(s)", nullptr, int(archive->entries.size()));
    if (!problems.isEmpty())
        description += QObject::tr("; %n entry(s) point outside the file", nullptr, int(problems.size()));
    table->setDescription(description);
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(table));
    return doc;
}
