#include "core/textdecode.h"

#include <QStringDecoder>
#include <QStringList>

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>

namespace {

constexpr char32_t kInvalid = 0xFFFFFFFF;

const char *icuName(TextEncoding e)
{
    switch (e) {
    case TextEncoding::ShiftJis: return "Shift_JIS";
    case TextEncoding::EucKr: return "EUC-KR";
    case TextEncoding::Gbk: return "GBK";
    case TextEncoding::Big5: return "Big5";
    case TextEncoding::Windows1252: return "windows-1252";
    default: return nullptr;
    }
}

QStringDecoder makeDecoder(TextEncoding e)
{
    switch (e) {
    case TextEncoding::Utf8: return QStringDecoder(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    case TextEncoding::Utf16LE: return QStringDecoder(QStringConverter::Utf16LE, QStringConverter::Flag::Stateless);
    case TextEncoding::Utf16BE: return QStringDecoder(QStringConverter::Utf16BE, QStringConverter::Flag::Stateless);
    default: return QStringDecoder(icuName(e), QStringConverter::Flag::Stateless);
    }
}

bool isLeadByte(TextEncoding e, uchar b)
{
    if (e == TextEncoding::ShiftJis)
        return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
    return b >= 0x81 && b <= 0xFE;
}

int legacyIndex(TextEncoding e)
{
    switch (e) {
    case TextEncoding::ShiftJis: return 0;
    case TextEncoding::EucKr: return 1;
    case TextEncoding::Gbk: return 2;
    default: return 3;
    }
}

using PairTable = std::array<char32_t, 65536>;

const PairTable &pairTable(TextEncoding e)
{
    static std::array<std::unique_ptr<PairTable>, 4> tables;
    static std::array<std::once_flag, 4> made;
    const int index = legacyIndex(e);
    std::call_once(made[size_t(index)], [&] {
        auto table = std::make_unique<PairTable>();
        table->fill(kInvalid);
        auto store = [&](int lead, int trail, QStringView s) {
            if (s.isEmpty() || s.contains(QChar::ReplacementCharacter))
                return;
            const char32_t cp = s.size() == 1 ? s[0].unicode()
                              : (s.size() == 2 && s[0].isHighSurrogate()) ? QChar::surrogateToUcs4(s[0], s[1])
                                                                          : kInvalid;
            (*table)[size_t(lead << 8 | trail)] = cp;
        };
        for (int lead = 0x81; lead <= 0xFE; ++lead) {
            if (!isLeadByte(e, uchar(lead)))
                continue;
            QByteArray batch;
            for (int trail = 0x40; trail <= 0xFE; ++trail)
                batch.append(char(lead)).append(char(trail)).append('\n');
            QStringDecoder decoder = makeDecoder(e);
            if (!decoder.isValid())
                break;
            const QString decoded = decoder.decode(batch);
            const QStringList pieces = decoded.split(u'\n');
            if (pieces.size() == 0xFE - 0x40 + 2) {
                for (int trail = 0x40; trail <= 0xFE; ++trail)
                    store(lead, trail, pieces[trail - 0x40]);
                continue;
            }
            for (int trail = 0x40; trail <= 0xFE; ++trail) {
                const char bytes[2] = {char(lead), char(trail)};
                QStringDecoder single = makeDecoder(e);
                const QString s = single.decode(QByteArrayView(bytes, 2));
                if (!single.hasError())
                    store(lead, trail, s);
            }
        }
        tables[size_t(index)] = std::move(table);
    });
    return *tables[size_t(index)];
}

constexpr char16_t kCp1252High[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

char32_t decodeCharacter(QByteArrayView data, qsizetype pos, TextEncoding e, int *length)
{
    const qsizetype left = data.size() - pos;
    const auto at = [&](qsizetype i) { return uchar(data[pos + i]); };
    *length = 1;
    const uchar b = at(0);
    switch (e) {
    case TextEncoding::Utf8: {
        if (b < 0x80)
            return b;
        int n = 0;
        char32_t cp = 0;
        if (b >= 0xC2 && b <= 0xDF) { n = 2; cp = b & 0x1F; }
        else if (b >= 0xE0 && b <= 0xEF) { n = 3; cp = b & 0x0F; }
        else if (b >= 0xF0 && b <= 0xF4) { n = 4; cp = b & 0x07; }
        else return kInvalid;
        if (left < n)
            return kInvalid;
        for (int i = 1; i < n; ++i) {
            if ((at(i) & 0xC0) != 0x80)
                return kInvalid;
            cp = cp << 6 | (at(i) & 0x3F);
        }
        if ((n == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)))
            return kInvalid;
        *length = n;
        return cp;
    }
    case TextEncoding::Utf16LE:
    case TextEncoding::Utf16BE: {
        if (left < 2)
            return kInvalid;
        const bool le = e == TextEncoding::Utf16LE;
        auto unit = [&](qsizetype i) { return char16_t(le ? at(i) | at(i + 1) << 8 : at(i) << 8 | at(i + 1)); };
        const char16_t u = unit(0);
        *length = 2;
        if (QChar::isLowSurrogate(u))
            return kInvalid;
        if (!QChar::isHighSurrogate(u))
            return u;
        if (left < 4 || !QChar::isLowSurrogate(unit(2)))
            return kInvalid;
        *length = 4;
        return QChar::surrogateToUcs4(u, unit(2));
    }
    case TextEncoding::Windows1252:
        if (b < 0x80 || b >= 0xA0)
            return b;
        return kCp1252High[b - 0x80] ? kCp1252High[b - 0x80] : kInvalid;
    default:
        if (b < 0x80)
            return b;
        if (e == TextEncoding::ShiftJis && b >= 0xA1 && b <= 0xDF)
            return 0xFF61 + (b - 0xA1);
        if (!isLeadByte(e, b) || left < 2)
            return kInvalid;
        if (const char32_t cp = pairTable(e)[size_t(b << 8 | at(1))]; cp != kInvalid) {
            *length = 2;
            return cp;
        }
        return kInvalid;
    }
}

bool isPrintableAscii(char32_t c)
{
    return (c >= 0x20 && c < 0x7F) || c == '\t' || c == '\n' || c == '\r';
}

bool isAsciiLetter(char32_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool inRange(char32_t c, char32_t from, char32_t to) { return c >= from && c <= to; }

bool isHan(char32_t c) { return inRange(c, 0x4E00, 0x9FFF); }
bool isKana(char32_t c) { return inRange(c, 0x3040, 0x30FF); }
bool isHangul(char32_t c) { return inRange(c, 0xAC00, 0xD7A3) || inRange(c, 0x3130, 0x318F); }
bool isCjkPunctuation(char32_t c) { return inRange(c, 0x3000, 0x303F) || inRange(c, 0xFF01, 0xFF5E); }

bool fitsScript(TextEncoding e, char32_t c)
{
    switch (e) {
    case TextEncoding::Utf16LE:
    case TextEncoding::Utf16BE:
        return isKana(c) || isHan(c) || isHangul(c) || isCjkPunctuation(c) || inRange(c, 0xA0, 0x24F)
            || inRange(c, 0x370, 0x4FF) || inRange(c, 0x2010, 0x2027);
    case TextEncoding::ShiftJis:
        return isKana(c) || isHan(c) || isCjkPunctuation(c) || inRange(c, 0xFF61, 0xFF9F);
    case TextEncoding::EucKr:
        return isHangul(c) || isHan(c) || isCjkPunctuation(c);
    case TextEncoding::Gbk:
    case TextEncoding::Big5:
        return isHan(c) || isCjkPunctuation(c);
    default:
        return c != kInvalid && QChar::isPrint(c) && !inRange(c, 0xE000, 0xF8FF);
    }
}

bool isCommon(TextEncoding e, char32_t c)
{
    static const std::u32string japanese = U"のにはをたがでてとしいるすまかんなこれさあうおくけっーンストルイクラ";
    static const std::u32string korean = U"이다의는에하을를고한지가서기로리사스니도";
    static const std::u32string simplified = U"的一是了我不人在他有这个们中来上大为和国说时要就出会也你对生能";
    static const std::u32string traditional = U"的一是了我不人在他有這個們中來上大為和國說時要就出會也你對生能";
    switch (e) {
    case TextEncoding::ShiftJis:
    case TextEncoding::Utf8:
    case TextEncoding::Utf16LE:
    case TextEncoding::Utf16BE:
        return japanese.find(c) != std::u32string::npos || korean.find(c) != std::u32string::npos
            || simplified.find(c) != std::u32string::npos || traditional.find(c) != std::u32string::npos;
    case TextEncoding::EucKr: return korean.find(c) != std::u32string::npos;
    case TextEncoding::Gbk: return simplified.find(c) != std::u32string::npos;
    case TextEncoding::Big5: return traditional.find(c) != std::u32string::npos;
    default: return false;
    }
}

struct Evidence
{
    qsizetype asciiRunBytes = 0;
    qsizetype strongRunBytes = 0;
    qsizetype nonAsciiRunChars = 0;
    int commonHits = 0;
};

Evidence measure(QByteArrayView data, TextEncoding e)
{
    Evidence ev;
    qsizetype runStart = 0, runChars = 0;
    bool runAsciiOnly = true, runStrong = false;
    int runCommon = 0, runMultibyte = 0, runNonAscii = 0, runLetters = 0;
    const bool wide = e == TextEncoding::Utf16LE || e == TextEncoding::Utf16BE;
    auto endRun = [&](qsizetype end) {
        if (runChars >= (wide ? 6 : 4)) {
            if (runAsciiOnly && e == TextEncoding::Utf8)
                ev.asciiRunBytes += end - runStart;
            if (wide && runCommon == 0 && runLetters * 2 < runChars)
                runStrong = false;
            if (runStrong)
                ev.strongRunBytes += end - runStart;
            ev.commonHits += runCommon;
            ev.nonAsciiRunChars += runNonAscii;
        }
        runChars = 0;
        runAsciiOnly = true;
        runStrong = false;
        runCommon = 0;
        runMultibyte = 0;
        runNonAscii = 0;
        runLetters = 0;
    };
    for (qsizetype pos = 0; pos < data.size();) {
        int length = 1;
        const char32_t c = decodeCharacter(data, pos, e, &length);
        const bool ascii = c != kInvalid && isPrintableAscii(c);
        const bool good = c != kInvalid && (ascii || fitsScript(e, c));
        if (!good) {
            endRun(pos);
            pos += wide ? 2 : 1;
            runStart = pos;
            continue;
        }
        if (runChars == 0)
            runStart = pos;
        ++runChars;
        if (!ascii) {
            runAsciiOnly = false;
            ++runNonAscii;
            if (isCommon(e, c)) {
                ++runCommon;
                runStrong = true;
            } else if (e == TextEncoding::Utf8 && ++runMultibyte >= 3) {
                runStrong = true;
            }
        } else if (wide && isAsciiLetter(c) && ++runLetters >= 4) {
            runStrong = true;
        }
        pos += length;
    }
    endRun(data.size());
    return ev;
}

int bomSize(QByteArrayView data, TextEncoding *encoding)
{
    if (data.startsWith("\xEF\xBB\xBF")) {
        *encoding = TextEncoding::Utf8;
        return 3;
    }
    if (data.startsWith("\xFF\xFE")) {
        *encoding = TextEncoding::Utf16LE;
        return 2;
    }
    if (data.startsWith("\xFE\xFF")) {
        *encoding = TextEncoding::Utf16BE;
        return 2;
    }
    return 0;
}

bool isValidUtf8Prefix(QByteArrayView data)
{
    if (QUtf8StringView(data).isValidUtf8())
        return true;
    for (int cut = 1; cut <= 3 && cut < data.size(); ++cut) {
        if (QUtf8StringView(data.chopped(cut)).isValidUtf8())
            return true;
    }
    return false;
}

} // namespace

QString encodingName(TextEncoding encoding)
{
    switch (encoding) {
    case TextEncoding::Utf8: return QStringLiteral("UTF-8");
    case TextEncoding::Utf16LE: return QStringLiteral("UTF-16 LE");
    case TextEncoding::Utf16BE: return QStringLiteral("UTF-16 BE");
    case TextEncoding::ShiftJis: return QStringLiteral("Shift-JIS (Japanese)");
    case TextEncoding::EucKr: return QStringLiteral("EUC-KR (Korean)");
    case TextEncoding::Gbk: return QStringLiteral("GBK (Simplified Chinese)");
    case TextEncoding::Big5: return QStringLiteral("Big5 (Traditional Chinese)");
    case TextEncoding::Windows1252: return QStringLiteral("Windows-1252 (Western)");
    }
    return {};
}

const std::vector<TextEncoding> &allTextEncodings()
{
    static const std::vector<TextEncoding> all = {
        TextEncoding::Utf8,  TextEncoding::Utf16LE, TextEncoding::Utf16BE, TextEncoding::ShiftJis,
        TextEncoding::EucKr, TextEncoding::Gbk,     TextEncoding::Big5,    TextEncoding::Windows1252,
    };
    return all;
}

QString decodeText(QByteArrayView bytes, TextEncoding encoding, bool *ok)
{
    QStringDecoder decoder = makeDecoder(encoding);
    if (!decoder.isValid()) {
        if (ok)
            *ok = false;
        return QString::fromLatin1(bytes);
    }
    QString text = decoder.decode(bytes);
    if (ok)
        *ok = !decoder.hasError() && (encoding == TextEncoding::Utf8 || encoding == TextEncoding::Utf16LE
                                      || encoding == TextEncoding::Utf16BE || !text.contains(QChar::ReplacementCharacter));
    return text;
}

int characterLength(QByteArrayView data, qsizetype pos, TextEncoding encoding)
{
    int length = 1;
    decodeCharacter(data, pos, encoding, &length);
    return length;
}

QString characterText(QByteArrayView data, qsizetype pos, TextEncoding encoding, int *length)
{
    const char32_t c = decodeCharacter(data, pos, encoding, length);
    if (c == kInvalid || c < 0x20 || (c >= 0x7F && c < 0xA0) || !QChar::isPrint(c))
        return QStringLiteral(".");
    return QString::fromUcs4(&c, 1);
}

bool looksLikeText(QByteArrayView sample)
{
    TextEncoding bom;
    if (bomSize(sample, &bom) > 0)
        return true;
    if (sample.isEmpty() || sample.contains('\0') || !isValidUtf8Prefix(sample))
        return false;
    qsizetype control = 0;
    for (char c : sample) {
        if (uchar(c) < 0x20 && c != '\t' && c != '\n' && c != '\r')
            ++control;
    }
    return control * 50 <= sample.size();
}

DetectedEncoding detectTextEncoding(QByteArrayView text)
{
    DetectedEncoding result;
    if ((result.bomSize = bomSize(text, &result.encoding)) > 0) {
        result.confident = true;
        return result;
    }
    const QByteArrayView sample = text.first(std::min<qsizetype>(text.size(), 256 * 1024));

    const qsizetype pairs = sample.size() / 2;
    qsizetype zeroEven = 0, zeroOdd = 0;
    for (qsizetype i = 0; i + 1 < sample.size(); i += 2) {
        zeroEven += sample[i] == '\0';
        zeroOdd += sample[i + 1] == '\0';
    }
    if (pairs >= 2 && zeroOdd * 10 > pairs * 3 && zeroEven * 20 < pairs) {
        result.encoding = TextEncoding::Utf16LE;
        result.confident = true;
        return result;
    }
    if (pairs >= 2 && zeroEven * 10 > pairs * 3 && zeroOdd * 20 < pairs) {
        result.encoding = TextEncoding::Utf16BE;
        result.confident = true;
        return result;
    }

    bool ascii = true;
    for (char c : sample) {
        if (uchar(c) >= 0x80) {
            ascii = false;
            break;
        }
    }
    if (ascii)
        return result;
    if (isValidUtf8Prefix(sample)) {
        result.confident = true;
        return result;
    }

    int bestHits = -1;
    double bestRatio = 0;
    for (TextEncoding e : {TextEncoding::ShiftJis, TextEncoding::EucKr, TextEncoding::Gbk, TextEncoding::Big5}) {
        qsizetype good = 0, bad = 0;
        int hits = 0;
        for (qsizetype pos = 0; pos < sample.size();) {
            int length = 1;
            const char32_t c = decodeCharacter(sample, pos, e, &length);
            if (c == kInvalid) {
                if (sample.size() - pos > 1)
                    ++bad;
            } else if (c >= 0x80) {
                if (fitsScript(e, c))
                    good += inRange(c, 0xFF61, 0xFF9F) ? 0 : 1;
                else
                    ++bad;
                hits += isCommon(e, c);
            }
            pos += length;
        }
        const double ratio = good + bad > 0 ? double(good) / double(good + bad) : 0;
        if (ratio < 0.9)
            continue;
        if (hits > bestHits || (hits == bestHits && ratio > bestRatio)) {
            bestHits = hits;
            bestRatio = ratio;
            result.encoding = e;
            result.confident = true;
        }
    }
    if (!result.confident)
        result.encoding = TextEncoding::Windows1252;
    return result;
}

DetectedEncoding detectEncodingInBinary(QByteArrayView data)
{
    DetectedEncoding result;
    if ((result.bomSize = bomSize(data, &result.encoding)) > 0) {
        result.confident = true;
        return result;
    }
    const QByteArrayView sample = data.first(std::min<qsizetype>(data.size(), 256 * 1024));
    const Evidence utf8 = measure(sample, TextEncoding::Utf8);

    qsizetype best16 = 0;
    for (TextEncoding e : {TextEncoding::Utf16LE, TextEncoding::Utf16BE}) {
        const Evidence ev = measure(sample, e);
        if (ev.strongRunBytes >= 64 && ev.strongRunBytes > utf8.asciiRunBytes + utf8.strongRunBytes
            && ev.strongRunBytes > best16) {
            best16 = ev.strongRunBytes;
            result.encoding = e;
            result.confident = true;
        }
    }
    if (result.confident)
        return result;

    int bestHits = 0;
    for (TextEncoding e : {TextEncoding::ShiftJis, TextEncoding::EucKr, TextEncoding::Gbk, TextEncoding::Big5}) {
        const Evidence ev = measure(sample, e);
        if (ev.commonHits >= 4 && ev.commonHits * 20 >= ev.nonAsciiRunChars && ev.strongRunBytes >= 32
            && ev.strongRunBytes > 2 * utf8.strongRunBytes
            && ev.commonHits > bestHits) {
            bestHits = ev.commonHits;
            result.encoding = e;
            result.confident = true;
        }
    }
    if (!result.confident)
        result.confident = utf8.strongRunBytes >= std::max<qsizetype>(64, sample.size() / 2048);
    return result;
}
