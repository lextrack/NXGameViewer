#include "data/tblfile.h"

#include "core/storedreader.h"

#include <QObject>
#include <QStringDecoder>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace {

constexpr int kHeaderSize = 8;
constexpr int kSectionHeaderSize = 80;
constexpr int kNameSize = 64;
constexpr quint32 kMaxSections = 10'000;
constexpr quint32 kMaxEntries = 10'000'000;

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

}

bool TblFile::loadFromFile(const QString &path, QString *error)
{
    QString e;
    const std::optional<QByteArray> bytes = readWholeFile(path, kMaxFileSize, &e);
    if (!bytes) {
        setError(error, QObject::tr("Cannot open %1: %2").arg(path, e));
        return false;
    }
    return load(*bytes, error);
}

bool TblFile::load(const QByteArray &data, QString *error)
{
    m_data.clear();
    m_sections.clear();
    m_poolStart = 0;
    m_encoding = TextEncoding::Utf8;

    if (data.size() < kHeaderSize || !data.startsWith("#TBL")) {
        setError(error, QObject::tr("Not a TBL file (missing #TBL header)."));
        return false;
    }

    const quint32 count = qFromLittleEndian<quint32>(data.constData() + 4);
    const quint64 headersEnd = kHeaderSize + quint64(count) * kSectionHeaderSize;
    if (count > kMaxSections || headersEnd > quint64(data.size())) {
        setError(error, QObject::tr("The section list is truncated or invalid."));
        return false;
    }

    QVector<TblSection> sections;
    quint64 dataEnd = headersEnd;
    quint64 totalData = 0;
    for (quint32 i = 0; i < count; ++i) {
        const char *h = data.constData() + kHeaderSize + qsizetype(i) * kSectionHeaderSize;
        TblSection s;
        s.name = QString::fromLatin1(h, qsizetype(qstrnlen(h, kNameSize)));
        s.hash = qFromLittleEndian<quint32>(h + 64);
        s.dataOffset = qFromLittleEndian<quint32>(h + 68);
        s.entrySize = qFromLittleEndian<quint32>(h + 72);
        s.entryCount = qFromLittleEndian<quint32>(h + 76);

        if (s.entrySize > kMaxEntrySize) {
            setError(error, QObject::tr("Section \"%1\" has entries of %2 bytes, which is not plausible.")
                                .arg(s.name).arg(s.entrySize));
            return false;
        }
        const quint64 end = quint64(s.dataOffset) + quint64(s.entrySize) * s.entryCount;
        if (s.dataOffset < headersEnd || end > quint64(data.size()) || s.entryCount > kMaxEntries) {
            setError(error, QObject::tr("Section \"%1\" points outside the file.").arg(s.name));
            return false;
        }
        totalData += quint64(s.entrySize) * s.entryCount;
        if (totalData > quint64(data.size()) - headersEnd) {
            setError(error, QObject::tr("Sections overlap each other."));
            return false;
        }
        dataEnd = std::max(dataEnd, end);
        sections.append(s);
    }

    m_data = data;
    m_poolStart = qsizetype(dataEnd);
    m_encoding = TextEncoding::Utf8;
    if (const DetectedEncoding detected = detectTextEncoding(QByteArrayView(m_data).sliced(m_poolStart));
        detected.confident && detected.encoding != TextEncoding::Utf16LE && detected.encoding != TextEncoding::Utf16BE)
        m_encoding = detected.encoding;
    m_sections = sections;
    for (TblSection &s : m_sections)
        inferColumns(s);
    return true;
}

QByteArrayView TblFile::entry(int section, int row) const
{
    Q_ASSERT(section >= 0 && section < m_sections.size());
    const TblSection &s = m_sections.at(section);
    Q_ASSERT(row >= 0 && quint32(row) < s.entryCount);
    return QByteArrayView(m_data).sliced(s.dataOffset + qsizetype(row) * s.entrySize, s.entrySize);
}

quint64 TblFile::u64At(qsizetype pos) const
{
    return qFromLittleEndian<quint64>(m_data.constData() + pos);
}

std::optional<QString> TblFile::textAt(quint64 offset) const
{
    const auto size = quint64(m_data.size());
    if (offset < quint64(m_poolStart) || offset >= size)
        return std::nullopt;
    if (offset > quint64(m_poolStart) && m_data.at(qsizetype(offset - 1)) != '\0')
        return std::nullopt;

    const char *begin = m_data.constData() + offset;
    const auto *end = static_cast<const char *>(std::memchr(begin, 0, size - offset));
    if (!end)
        return std::nullopt;

    bool ok = false;
    const QString text = decodeText(QByteArrayView(begin, end), m_encoding, &ok);
    if (!ok)
        return std::nullopt;
    for (QChar c : std::as_const(text)) {
        if (c.unicode() < 0x20 && c != u'\n' && c != u'\r' && c != u'\t')
            return std::nullopt;
    }
    return text;
}

void TblFile::inferColumns(TblSection &s) const
{
    const int size = int(s.entrySize);
    int offset = 0;
    while (offset < size) {
        if (offset + 8 <= size && s.entryCount > 0) {
            bool allValid = true;
            quint32 hits = 0;
            for (quint32 row = 0; row < s.entryCount && allValid; ++row) {
                const quint64 v = u64At(s.dataOffset + qsizetype(row) * size + offset);
                if (v == 0)
                    continue;
                if (textAt(v))
                    ++hits;
                else
                    allValid = false;
            }
            if (allValid && hits > 0) {
                s.columns.append({offset, ColumnType::String});
                offset += 8;
                continue;
            }
        }
        if (offset + 4 <= size) {
            s.columns.append({offset, ColumnType::U32});
            offset += 4;
        } else {
            s.columns.append({offset, ColumnType::Byte});
            offset += 1;
        }
    }
}
