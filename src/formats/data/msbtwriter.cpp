#include "data/msbtwriter.h"

#include "data/msbtreader.h"

#include <QHash>
#include <QObject>
#include <QRegularExpression>
#include <QtEndian>

#include <vector>

namespace {

constexpr qsizetype kHeaderSize = 0x20;

struct Section
{
    QByteArray magic;
    qsizetype header = 0;
    qsizetype start = 0;
    qsizetype size = 0;
    qsizetype next = 0;
};

class Encoder
{
public:
    Encoder(bool little, int encoding) : m_little(little), m_unit(encoding == 0 ? 1 : encoding == 1 ? 2 : 4) {}

    std::optional<QByteArray> encode(const QString &text, QString *problem) const
    {
        static const QRegularExpression tag(QStringLiteral(R"(\[(?:(page)|/(\d+):(\d+)|(\d+):(\d+)(?: ([0-9A-Fa-f]*))?)\])"));
        QByteArray out;
        qsizetype at = 0;
        while (at < text.size()) {
            const QRegularExpressionMatch m = text.at(at) == u'['
                                                ? tag.match(text, at, QRegularExpression::NormalMatch,
                                                            QRegularExpression::AnchorAtOffsetMatchOption)
                                                : QRegularExpressionMatch();
            if (m.hasMatch()) {
                if (!m.captured(1).isEmpty()) {
                    unit(out, 0x0E);
                    u16(out, 0);
                    u16(out, 4);
                    u16(out, 0);
                } else if (!m.captured(2).isEmpty()) {
                    unit(out, 0x0F);
                    if (!field(out, m.captured(2)) || !field(out, m.captured(3)))
                        return bad(problem, m.captured(0));
                } else {
                    const QString hex = m.captured(6);
                    if (hex.size() % 2 || hex.size() / 2 > 0xFFFF)
                        return bad(problem, m.captured(0));
                    unit(out, 0x0E);
                    if (!field(out, m.captured(4)) || !field(out, m.captured(5)))
                        return bad(problem, m.captured(0));
                    const QByteArray params = QByteArray::fromHex(hex.toLatin1());
                    u16(out, quint16(params.size()));
                    out += params;
                }
                at = m.capturedEnd(0);
                continue;
            }
            if (m_unit == 1) {
                qsizetype end = at;
                while (end < text.size() && text.at(end) != u'[')
                    ++end;
                if (end == at)
                    ++end;
                out += text.mid(at, end - at).toUtf8();
                at = end;
            } else if (m_unit == 2) {
                unit(out, text.at(at).unicode());
                ++at;
            } else {
                char32_t c = text.at(at).unicode();
                if (text.at(at).isHighSurrogate() && at + 1 < text.size() && text.at(at + 1).isLowSurrogate()) {
                    c = QChar::surrogateToUcs4(text.at(at), text.at(at + 1));
                    ++at;
                }
                unit(out, c);
                ++at;
            }
        }
        unit(out, 0);
        return out;
    }

private:
    static std::optional<QByteArray> bad(QString *problem, const QString &tag)
    {
        if (problem)
            *problem = QObject::tr("the tag %1 cannot be written").arg(tag);
        return std::nullopt;
    }
    void unit(QByteArray &out, quint32 c) const
    {
        char b[4];
        if (m_unit == 1) {
            out += char(c);
        } else if (m_unit == 2) {
            m_little ? qToLittleEndian(quint16(c), b) : qToBigEndian(quint16(c), b);
            out.append(b, 2);
        } else {
            m_little ? qToLittleEndian(c, b) : qToBigEndian(c, b);
            out.append(b, 4);
        }
    }
    void u16(QByteArray &out, quint16 v) const
    {
        char b[2];
        m_little ? qToLittleEndian(v, b) : qToBigEndian(v, b);
        out.append(b, 2);
    }
    bool field(QByteArray &out, const QString &digits) const
    {
        bool ok = false;
        const uint v = digits.toUInt(&ok);
        if (!ok || v > 0xFFFF)
            return false;
        u16(out, quint16(v));
        return true;
    }

    bool m_little;
    int m_unit;
};

std::optional<QByteArray> fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return std::nullopt;
}

}

std::optional<QByteArray> msbtFromJson(const QJsonObject &edited, const QByteArray &original, QString *error)
{
    const std::optional<std::vector<MsbtMessage>> messages = MsbtReader::parse(original, error);
    if (!messages)
        return std::nullopt;
    const bool little = uchar(original[8]) == 0xFF && uchar(original[9]) == 0xFE;
    auto u32 = [&](qsizetype at) {
        return little ? qFromLittleEndian<quint32>(original.constData() + at) : qFromBigEndian<quint32>(original.constData() + at);
    };
    auto put32 = [little](QByteArray &b, qsizetype at, quint32 v) {
        little ? qToLittleEndian(v, b.data() + at) : qToBigEndian(v, b.data() + at);
    };
    const int encoding = quint8(original[0x0C]);
    const int sectionCount = little ? qFromLittleEndian<quint16>(original.constData() + 0x0E)
                                    : qFromBigEndian<quint16>(original.constData() + 0x0E);

    std::vector<Section> sections;
    qsizetype at = kHeaderSize;
    for (int i = 0; i < sectionCount && at + 16 <= original.size(); ++i) {
        Section s{original.mid(at, 4), at, at + 16, qsizetype(u32(at + 4)), 0};
        s.next = std::min<qsizetype>((s.start + s.size + 15) / 16 * 16, original.size());
        sections.push_back(s);
        at = s.next;
    }
    const Section *txt = nullptr;
    for (const Section &s : sections)
        if (s.magic == "TXT2")
            txt = &s;
    if (!txt)
        return fail(error, QObject::tr("This MSBT file has no text section."));
    const quint32 count = u32(txt->start);

    QHash<QString, int> byKey;
    for (const MsbtMessage &m : *messages)
        byKey.insert(m.label.isEmpty() ? QString::number(m.index) : m.label, m.index);
    for (auto it = edited.begin(); it != edited.end(); ++it) {
        if (!byKey.contains(it.key()))
            return fail(error, QObject::tr("\"%1\" is not a message in the game's file. Messages can be changed, "
                                           "but not added or renamed.").arg(it.key()));
        if (!it.value().isString())
            return fail(error, QObject::tr("\"%1\": the message should be text in quotes.").arg(it.key()));
    }

    const Encoder encoder(little, encoding);
    QByteArray body;
    std::vector<quint32> offsets;
    const qsizetype tableSize = 4 + qsizetype(count) * 4;
    for (quint32 i = 0; i < count; ++i) {
        const qsizetype begin = txt->start + u32(txt->start + 4 + qsizetype(i) * 4);
        const qsizetype end = i + 1 < count ? txt->start + u32(txt->start + 8 + qsizetype(i) * 4) : txt->start + txt->size;
        offsets.push_back(quint32(tableSize + body.size()));
        const MsbtMessage &m = (*messages)[i];
        const QString key = m.label.isEmpty() ? QString::number(m.index) : m.label;
        const auto found = edited.constFind(key);
        if (found == edited.constEnd() || found->toString() == m.text) {
            if (begin < txt->start || begin > end || end > txt->start + txt->size)
                return fail(error, QObject::tr("Message %1 of the game's file is damaged.").arg(i));
            body += original.mid(begin, end - begin);
            continue;
        }
        QString problem;
        const std::optional<QByteArray> encoded = encoder.encode(found->toString(), &problem);
        if (!encoded)
            return fail(error, QObject::tr("\"%1\": %2.").arg(key, problem));
        body += *encoded;
    }
    QByteArray section(tableSize, '\0');
    put32(section, 0, count);
    for (quint32 i = 0; i < count; ++i)
        put32(section, 4 + qsizetype(i) * 4, offsets[i]);
    section += body;

    QByteArray out = original.left(kHeaderSize);
    for (const Section &s : sections) {
        if (&s != txt) {
            out += original.mid(s.header, s.next - s.header);
            continue;
        }
        QByteArray head = original.mid(s.header, 16);
        put32(head, 4, quint32(section.size()));
        out += head + section;
        while (out.size() % 16)
            out += char(0xAB);
    }
    out += original.mid(sections.back().next);
    put32(out, 0x12, quint32(out.size()));
    return out;
}
