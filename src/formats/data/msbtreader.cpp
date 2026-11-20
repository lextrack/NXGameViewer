#include "data/msbtreader.h"

#include "core/simpletable.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QtEndian>

#include <algorithm>
#include <map>

namespace {

constexpr qsizetype kHeaderSize = 0x20;
constexpr int kMaxItems = 1'000'000;

class Reader
{
public:
    Reader(const QByteArray &data, bool littleEndian) : m_data(data), m_little(littleEndian) {}

    bool fits(qsizetype at, qsizetype size) const { return at >= 0 && size >= 0 && at <= m_data.size() - size; }
    quint16 u16(qsizetype at) const
    {
        return m_little ? qFromLittleEndian<quint16>(m_data.constData() + at) : qFromBigEndian<quint16>(m_data.constData() + at);
    }
    quint32 u32(qsizetype at) const
    {
        return m_little ? qFromLittleEndian<quint32>(m_data.constData() + at) : qFromBigEndian<quint32>(m_data.constData() + at);
    }
    const QByteArray &data() const { return m_data; }

private:
    const QByteArray &m_data;
    bool m_little;
};

struct Section
{
    QByteArray magic;
    qsizetype start = 0;
    qsizetype size = 0;
};

QString tagText(int group, int type, const QByteArray &params, bool closing)
{
    if (closing)
        return QStringLiteral("[/%1:%2]").arg(group).arg(type);
    if (group == 0 && type == 4 && params.isEmpty())
        return QStringLiteral("[page]");
    QString t = QStringLiteral("[%1:%2").arg(group).arg(type);
    if (!params.isEmpty())
        t += u' ' + QString::fromLatin1(params.toHex());
    return t + u']';
}

QString decodeMessage(const Reader &r, qsizetype start, qsizetype end, int encoding)
{
    const int unit = encoding == 0 ? 1 : encoding == 1 ? 2 : 4;
    QString out;
    QByteArray utf8;
    auto flushUtf8 = [&] {
        if (!utf8.isEmpty())
            out += QString::fromUtf8(utf8);
        utf8.clear();
    };
    qsizetype at = start;
    while (at + unit <= end) {
        const quint32 c = unit == 1 ? quint8(r.data()[at]) : unit == 2 ? r.u16(at) : r.u32(at);
        at += unit;
        if (c == 0)
            break;
        if (c == 0x0E || c == 0x0F) {
            flushUtf8();
            const bool closing = c == 0x0F;
            if (!r.fits(at, closing ? 4 : 6) || at + (closing ? 4 : 6) > end)
                break;
            const int group = r.u16(at), type = r.u16(at + 2);
            at += 4;
            QByteArray params;
            if (!closing) {
                const int size = r.u16(at);
                at += 2;
                if (at + size > end)
                    break;
                params = r.data().mid(at, size);
                at += size;
            }
            out += tagText(group, type, params, closing);
            continue;
        }
        if (unit == 1) {
            utf8.append(char(c));
        } else if (unit == 2) {
            out += QChar(char16_t(c));
        } else {
            const char32_t u = char32_t(c);
            out += QString::fromUcs4(&u, 1);
        }
    }
    flushUtf8();
    return out;
}

}

std::optional<std::vector<MsbtMessage>> MsbtReader::parse(const QByteArray &data, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<std::vector<MsbtMessage>> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (data.size() < kHeaderSize || !data.startsWith("MsgStdBn"))
        return fail(QObject::tr("Not an MSBT file (no \"MsgStdBn\" at the start)."));
    const bool little = uchar(data[8]) == 0xFF && uchar(data[9]) == 0xFE;
    const Reader r(data, little);
    const int encoding = quint8(data[0x0C]);
    if (encoding > 2)
        return fail(QObject::tr("Unknown MSBT text encoding %1.").arg(encoding));
    const int sectionCount = r.u16(0x0E);

    std::vector<Section> sections;
    qsizetype at = kHeaderSize;
    for (int i = 0; i < sectionCount && r.fits(at, 16); ++i) {
        Section s{data.mid(at, 4), at + 16, r.u32(at + 4)};
        if (!r.fits(s.start, s.size))
            return fail(QObject::tr("The %1 section runs past the end of the file.").arg(QString::fromLatin1(s.magic)));
        sections.push_back(s);
        at = (s.start + s.size + 15) / 16 * 16;
    }
    auto find = [&](const char *magic) -> const Section * {
        for (const Section &s : sections)
            if (s.magic == magic)
                return &s;
        return nullptr;
    };

    std::vector<MsbtMessage> messages;
    if (const Section *txt = find("TXT2")) {
        if (txt->size < 4)
            return fail(QObject::tr("The TXT2 section is too short."));
        const quint32 count = r.u32(txt->start);
        if (count > quint32(kMaxItems) || qint64(count) * 4 + 4 > txt->size)
            return fail(QObject::tr("The TXT2 section has an impossible message count."));
        for (quint32 i = 0; i < count; ++i) {
            const qsizetype begin = txt->start + r.u32(txt->start + 4 + qsizetype(i) * 4);
            const qsizetype end = i + 1 < count ? txt->start + r.u32(txt->start + 8 + qsizetype(i) * 4)
                                                : txt->start + txt->size;
            MsbtMessage m;
            m.index = int(i);
            if (begin >= txt->start && begin <= end && end <= txt->start + txt->size)
                m.text = decodeMessage(r, begin, end, encoding);
            messages.push_back(std::move(m));
        }
    }
    if (const Section *lbl = find("LBL1"); lbl && lbl->size >= 4) {
        const quint32 groups = r.u32(lbl->start);
        for (quint32 g = 0; g < groups && g < quint32(kMaxItems) && qint64(g) * 8 + 12 <= lbl->size; ++g) {
            const quint32 labels = r.u32(lbl->start + 4 + qsizetype(g) * 8);
            qsizetype p = lbl->start + r.u32(lbl->start + 8 + qsizetype(g) * 8);
            for (quint32 k = 0; k < labels && k < quint32(kMaxItems); ++k) {
                if (!r.fits(p, 1) || p >= lbl->start + lbl->size)
                    break;
                const int length = quint8(data[p]);
                if (p + 1 + length + 4 > lbl->start + lbl->size)
                    break;
                const QString label = QString::fromUtf8(data.constData() + p + 1, length);
                const quint32 index = r.u32(p + 1 + length);
                p += 1 + length + 4;
                if (index >= messages.size())
                    messages.resize(size_t(index) + 1);
                messages[index].index = int(index);
                messages[index].label = label;
            }
        }
    }
    if (const Section *atr = find("ATR1"); atr && atr->size >= 8) {
        const quint32 count = r.u32(atr->start);
        const quint32 size = r.u32(atr->start + 4);
        if (size > 0 && size <= 256 && qint64(count) * size + 8 <= atr->size)
            for (quint32 i = 0; i < count && i < messages.size(); ++i)
                messages[i].attribute = data.mid(atr->start + 8 + qsizetype(i) * size, size);
    }
    return messages;
}

int MsbtReader::score(const QString &, QByteArrayView head) const
{
    return head.startsWith("MsgStdBn") ? 100 : 0;
}

std::unique_ptr<Document> MsbtReader::read(const QString &path, QString *error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return nullptr;
    }
    if (file.size() > kMaxFileSize) {
        if (error)
            *error = QObject::tr("File is larger than 64 MB.");
        return nullptr;
    }
    return readBytes(file.readAll(), path, error);
}

std::unique_ptr<Document> MsbtReader::readBytes(const QByteArray &data, const QString &fileName, QString *error) const
{
    std::optional<std::vector<MsbtMessage>> messages = parse(data, error);
    if (!messages)
        return nullptr;
    const bool attributes = std::any_of(messages->begin(), messages->end(),
                                        [](const MsbtMessage &m) { return !m.attribute.isEmpty(); });
    std::vector<SimpleTable::Row> rows;
    for (const MsbtMessage &m : *messages) {
        SimpleTable::Row row{m.index, m.label, QString(m.text).replace(u'\n', QStringLiteral(" ↵ "))};
        if (attributes)
            row.push_back(QString::fromLatin1(m.attribute.toHex(' ')));
        rows.push_back(std::move(row));
    }
    std::vector<SimpleColumn> columns{{QObject::tr("index")}, {QObject::tr("label")}, {QObject::tr("text")}};
    if (attributes)
        columns.push_back({QObject::tr("attributes")});
    QString name = QFileInfo(fileName).completeBaseName();
    auto table = std::make_shared<SimpleTable>(name.isEmpty() ? QStringLiteral("Messages") : name, std::move(columns),
                                               std::move(rows));
    table->setDescription(QObject::tr("%n message(s)", nullptr, int(messages->size())));
    table->setJsonExporter([messages = std::move(*messages)] {
        QJsonObject out;
        for (const MsbtMessage &m : messages)
            out.insert(m.label.isEmpty() ? QString::number(m.index) : m.label, m.text);
        return QJsonDocument(out).toJson();
    });
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(table));
    return doc;
}
