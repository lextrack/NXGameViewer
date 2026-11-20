#include "data/hexreader.h"

#include <QFile>
#include <QLocale>
#include <QObject>

namespace {

constexpr int kBytesPerRow = 16;

class HexTable : public Table
{
public:
    HexTable(QByteArray bytes, qint64 fileSize, TextEncoding encoding, bool detected)
        : m_bytes(std::move(bytes)), m_fileSize(fileSize), m_encoding(encoding), m_detected(detected)
    {
        m_skip.assign(size_t(rowCount()), 0);
        const QByteArrayView view(m_bytes);
        for (qsizetype pos = 0; pos < view.size();) {
            const qsizetype next = pos + characterLength(view, pos, m_encoding);
            for (qsizetype boundary = (pos / kBytesPerRow + 1) * kBytesPerRow; boundary < next && boundary < view.size();
                 boundary += kBytesPerRow)
                m_skip[size_t(boundary / kBytesPerRow)] = quint8(next - boundary);
            pos = next;
        }
    }

    QString name() const override { return QStringLiteral("Bytes"); }
    int rowCount() const override { return int((m_bytes.size() + kBytesPerRow - 1) / kBytesPerRow); }
    int columnCount() const override { return 2; }
    QString columnName(int c) const override
    {
        return c == 0 ? QStringLiteral("00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F")
                      : QStringLiteral("text");
    }
    QString columnToolTip(int c) const override
    {
        return c == 1 ? QObject::tr("Bytes read as %1 text").arg(encodingName(m_encoding)) : QString();
    }

    QString text(int r, int c, DisplayMode) const override
    {
        Q_ASSERT(r >= 0 && r < rowCount());
        const qsizetype start = qsizetype(r) * kBytesPerRow;
        const qsizetype end = std::min<qsizetype>(start + kBytesPerRow, m_bytes.size());
        QString out;
        if (c == 0) {
            for (qsizetype i = start; i < end; ++i) {
                if (i - start == 8)
                    out += ' ';
                out += QStringLiteral("%1 ").arg(quint8(m_bytes[i]), 2, 16, QLatin1Char('0')).toUpper();
            }
            return out.trimmed();
        }
        const QByteArrayView view(m_bytes);
        for (qsizetype pos = start + m_skip[size_t(r)]; pos < end;) {
            int length = 1;
            out += characterText(view, pos, m_encoding, &length);
            pos += length;
        }
        return out;
    }

    QString rowLabel(int r) const override
    {
        return QStringLiteral("0x%1").arg(qint64(r) * kBytesPerRow, 8, 16, QLatin1Char('0')).toUpper().replace("0X", "0x");
    }
    bool monospace() const override { return true; }

    QString description() const override
    {
        const QLocale locale;
        QString d = locale.formattedDataSize(m_fileSize);
        if (m_bytes.size() < m_fileSize)
            d += QObject::tr(" (showing the first %1)").arg(locale.formattedDataSize(m_bytes.size()));
        d += QObject::tr(", text as %1").arg(encodingName(m_encoding));
        if (m_detected && m_encoding != TextEncoding::Utf8)
            d += QObject::tr(" (detected)");
        return d;
    }

    bool canChangeTextEncoding() const override { return true; }

    std::shared_ptr<const Table> withTextEncoding(TextEncoding encoding) const override
    {
        return std::make_shared<HexTable>(m_bytes, m_fileSize, encoding, false);
    }

private:
    QByteArray m_bytes;
    qint64 m_fileSize;
    TextEncoding m_encoding;
    bool m_detected;
    std::vector<quint8> m_skip;
};

}

std::unique_ptr<Document> HexReader::read(const QString &path, QString *error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return nullptr;
    }
    QByteArray bytes = file.read(kMaxBytes);
    const DetectedEncoding detected = detectEncodingInBinary(bytes);
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::make_shared<HexTable>(std::move(bytes), file.size(), detected.encoding, detected.confident));
    return doc;
}

std::unique_ptr<Document> HexReader::readStored(const StoredReader &read, qint64 size, const QString &,
                                                QString *error) const
{
    QByteArray bytes = read(0, std::min(size, kMaxBytes));
    if (bytes.isEmpty() && size > 0) {
        if (error)
            *error = QObject::tr("The file cannot be read.");
        return nullptr;
    }
    const DetectedEncoding detected = detectEncodingInBinary(bytes);
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::make_shared<HexTable>(std::move(bytes), size, detected.encoding, detected.confident));
    return doc;
}

std::unique_ptr<Document> HexReader::readBytes(const QByteArray &data, const QString &, QString *) const
{
    QByteArray bytes = data.left(kMaxBytes);
    const DetectedEncoding detected = detectEncodingInBinary(bytes);
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::make_shared<HexTable>(std::move(bytes), data.size(), detected.encoding, detected.confident));
    return doc;
}
