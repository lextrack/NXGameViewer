#include "data/nintendofont.h"

#include <QObject>
#include <QStringDecoder>
#include <QtEndian>

#include <array>

namespace {

constexpr std::array<uchar, 4> kPlainMagic{0x7F, 0x9A, 0x02, 0x18};
constexpr qsizetype kHeaderSize = 8;
constexpr qsizetype kMaxPadding = 64;

const std::array<QByteArray, 4> &signatures()
{
    static const std::array<QByteArray, 4> s{QByteArray("\x00\x01\x00\x00", 4), QByteArray("OTTO"), QByteArray("true"),
                                             QByteArray("ttcf")};
    return s;
}

std::array<uchar, 4> keyFromMagic(QByteArrayView data)
{
    std::array<uchar, 4> key{};
    for (int i = 0; i < 4; ++i)
        key[size_t(i)] = uchar(data[i]) ^ kPlainMagic[size_t(i)];
    return key;
}

bool sizeMatches(QByteArrayView data, const std::array<uchar, 4> &key, qint64 available)
{
    uchar raw[4];
    for (int i = 0; i < 4; ++i)
        raw[i] = uchar(data[4 + i]) ^ key[size_t(i)];
    for (const quint32 size : {qFromBigEndian<quint32>(raw), qFromLittleEndian<quint32>(raw)})
        if (qint64(size) <= available && qint64(size) > available - kMaxPadding && size >= 12)
            return true;
    return false;
}

bool decryptsToFont(QByteArrayView data, const std::array<uchar, 4> &key)
{
    QByteArray head(4, '\0');
    for (int i = 0; i < 4; ++i)
        head[i] = char(uchar(data[kHeaderSize + i]) ^ key[size_t(i)]);
    for (const QByteArray &s : signatures())
        if (head == s)
            return true;
    return false;
}

quint16 be16(const QByteArray &d, qsizetype at)
{
    return at >= 0 && at + 2 <= d.size() ? qFromBigEndian<quint16>(d.constData() + at) : 0;
}

quint32 be32(const QByteArray &d, qsizetype at)
{
    return at >= 0 && at + 4 <= d.size() ? qFromBigEndian<quint32>(d.constData() + at) : 0;
}

QString nameString(const QByteArray &d, qsizetype at, int length, int platform)
{
    if (at < 0 || length <= 0 || at + length > d.size())
        return {};
    if (platform == 0 || platform == 3) {
        QStringDecoder utf16(QStringDecoder::Utf16BE);
        return QString(utf16(QByteArrayView(d).sliced(at, length)));
    }
    return QString::fromLatin1(d.constData() + at, length);
}

}

bool isSfnt(QByteArrayView head)
{
    if (head.size() < 4)
        return false;
    for (const QByteArray &s : signatures())
        if (head.startsWith(s))
            return true;
    return false;
}

bool isNintendoFont(QByteArrayView head)
{
    return head.size() >= kHeaderSize + 4 && decryptsToFont(head, keyFromMagic(head));
}

std::optional<QByteArray> decryptNintendoFont(QByteArrayView data)
{
    if (data.size() < kHeaderSize + 12)
        return std::nullopt;
    const qint64 available = data.size() - kHeaderSize;
    std::optional<std::array<uchar, 4>> key;
    const std::array<uchar, 4> fromMagic = keyFromMagic(data);
    if (decryptsToFont(data, fromMagic) && sizeMatches(data, fromMagic, available))
        key = fromMagic;
    for (size_t s = 0; !key && s < signatures().size(); ++s) {
        std::array<uchar, 4> guess{};
        for (int i = 0; i < 4; ++i)
            guess[size_t(i)] = uchar(data[kHeaderSize + i]) ^ uchar(signatures()[s][i]);
        if (sizeMatches(data, guess, available))
            key = guess;
    }
    if (!key)
        return std::nullopt;
    uchar raw[4];
    for (int i = 0; i < 4; ++i)
        raw[i] = uchar(data[4 + i]) ^ (*key)[size_t(i)];
    qint64 size = qFromBigEndian<quint32>(raw);
    if (size > available || size <= available - kMaxPadding)
        size = qFromLittleEndian<quint32>(raw);
    QByteArray out(qsizetype(size), Qt::Uninitialized);
    const auto *in = reinterpret_cast<const uchar *>(data.data()) + kHeaderSize;
    for (qsizetype i = 0; i < out.size(); ++i)
        out[i] = char(in[i] ^ (*key)[size_t(i % 4)]);
    return out;
}

std::optional<SfntInfo> sfntInfo(const QByteArray &font)
{
    if (!isSfnt(font) || font.size() < 12)
        return std::nullopt;
    SfntInfo info;
    qsizetype base = 0;
    if (font.startsWith("ttcf")) {
        info.fonts = int(be32(font, 8));
        base = be32(font, 12);
        if (info.fonts <= 0 || base <= 0 || base + 12 > font.size())
            return std::nullopt;
    }
    const int tableCount = be16(font, base + 4);
    if (tableCount <= 0 || base + 12 + qsizetype(tableCount) * 16 > font.size())
        return std::nullopt;
    qsizetype name = -1, maxp = -1;
    for (int i = 0; i < tableCount; ++i) {
        const qsizetype record = base + 12 + qsizetype(i) * 16;
        const QString tag = QString::fromLatin1(font.constData() + record, 4);
        const qsizetype offset = be32(font, record + 8);
        const qsizetype length = be32(font, record + 12);
        if (offset < 0 || length < 0 || offset > font.size() - length)
            continue;
        info.tables << tag.trimmed();
        if (tag == QLatin1String("name"))
            name = offset;
        else if (tag == QLatin1String("maxp"))
            maxp = offset;
    }
    info.outlines = info.tables.contains(QLatin1String("CFF")) || info.tables.contains(QLatin1String("CFF2"))
                        ? QObject::tr("PostScript (CFF)")
                        : QObject::tr("TrueType");
    if (maxp >= 0)
        info.glyphs = be16(font, maxp + 4);
    if (name >= 0) {
        const int count = be16(font, name + 2);
        const qsizetype strings = name + be16(font, name + 4);
        std::array<int, 7> rank{};
        std::array<QString, 7> found;
        for (int i = 0; i < count; ++i) {
            const qsizetype r = name + 6 + qsizetype(i) * 12;
            if (r + 12 > font.size())
                break;
            const int platform = be16(font, r), language = be16(font, r + 4), id = be16(font, r + 6);
            if (id < 1 || id > 6)
                continue;
            const int score = platform == 3 && language == 0x409 ? 4 : platform == 3 ? 3 : platform == 0 ? 2 : 1;
            if (score <= rank[size_t(id)])
                continue;
            const QString text = nameString(font, strings + be16(font, r + 10), be16(font, r + 8), platform);
            if (text.isEmpty())
                continue;
            rank[size_t(id)] = score;
            found[size_t(id)] = text;
        }
        info.family = found[1];
        info.style = found[2];
        info.fullName = found[4];
        info.version = found[5];
        info.postScriptName = found[6];
    }
    return info;
}

QString sfntSuffix(const QByteArray &font)
{
    if (font.startsWith("ttcf"))
        return QStringLiteral("ttc");
    if (font.startsWith("OTTO"))
        return QStringLiteral("otf");
    return QStringLiteral("ttf");
}
