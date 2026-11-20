#include "data/falcomtblreader.h"
#include "core/records.h"
#include "data/tblfile.h"

#include <QObject>

namespace {

class TblRecords : public RecordSource
{
public:
    TblRecords(std::shared_ptr<const TblFile> file, int section)
        : m_file(std::move(file)), m_section(section) {}

    QString recordTypeName() const override { return sec().name; }
    int recordSize() const override { return int(sec().entrySize); }
    int recordCount() const override { return int(sec().entryCount); }
    QByteArrayView record(int row) const override { return m_file->entry(m_section, row); }

    std::vector<FieldDef> guessFields() const override
    {
        std::vector<FieldDef> fields;
        fields.reserve(size_t(sec().columns.size()));
        for (const TblColumn &c : sec().columns) {
            const FieldType type = c.type == ColumnType::String ? FieldType::Text
                                 : c.type == ColumnType::U32    ? FieldType::Auto32
                                                                : FieldType::U8;
            fields.push_back({c.offset, type, {}, false});
        }
        return fields;
    }

    bool hasTextPointers() const override { return true; }
    std::optional<QString> textAt(quint64 pointer) const override { return m_file->textAt(pointer); }
    QString description() const override
    {
        QString d = QObject::tr("at 0x%1").arg(QString::number(sec().dataOffset, 16).toUpper());
        if (m_file->textEncoding() != TextEncoding::Utf8)
            d += QObject::tr(", text as %1").arg(encodingName(m_file->textEncoding()));
        return d;
    }

private:
    const TblSection &sec() const { return m_file->sections().at(m_section); }

    std::shared_ptr<const TblFile> m_file;
    int m_section;
};

std::unique_ptr<Document> makeDocument(const std::shared_ptr<const TblFile> &file)
{
    auto doc = std::make_unique<Document>();
    for (int i = 0; i < file->sections().size(); ++i)
        doc->tables.push_back(std::make_shared<RecordTable>(std::make_shared<TblRecords>(file, i)));
    return doc;
}

}

int FalcomTblReader::score(const QString &, QByteArrayView head) const
{
    return head.startsWith("#TBL") ? 100 : 0;
}

std::unique_ptr<Document> FalcomTblReader::read(const QString &path, QString *error) const
{
    auto file = std::make_shared<TblFile>();
    if (!file->loadFromFile(path, error))
        return nullptr;
    return makeDocument(file);
}

std::unique_ptr<Document> FalcomTblReader::fromBytes(const QByteArray &data, QString *error)
{
    auto file = std::make_shared<TblFile>();
    if (!file->load(data, error))
        return nullptr;
    return makeDocument(file);
}
