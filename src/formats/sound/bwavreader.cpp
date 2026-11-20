#include "sound/bwavreader.h"

#include "archives/zstddictionaries.h"
#include "core/simpletable.h"
#include "sound/nxopusreader.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QtEndian>

#include <algorithm>
#include <memory>

namespace {

constexpr qsizetype kHeaderSize = 0x10;
constexpr qsizetype kChannelSize = 0x4C;
constexpr int kSamplesPerFrame = 14;
constexpr int kBytesPerFrame = 8;

qint64 channelBytes(const BwavChannel &c)
{
    if (c.codec == 0)
        return c.samples * 2;
    return (c.samples + kSamplesPerFrame - 1) / kSamplesPerFrame * kBytesPerFrame;
}

QString duration(qint64 samples, int rate)
{
    if (rate <= 0)
        return {};
    const qint64 ms = samples * 1000 / rate;
    return QStringLiteral("%1:%2.%3")
        .arg(ms / 60000)
        .arg(ms / 1000 % 60, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

QString codecName(int codec)
{
    switch (codec) {
    case 0: return QStringLiteral("PCM 16-bit");
    case 1: return QStringLiteral("DSP-ADPCM");
    case 2: return QStringLiteral("Opus");
    default: return QObject::tr("unknown (%1)").arg(codec);
    }
}

constexpr int kMaxStreamFolders = 64;

QMutex streamMutex;
QHash<QString, QStringList> streamFolders;

QStringList streamFoldersUnder(const QString &root)
{
    {
        const QMutexLocker lock(&streamMutex);
        const auto it = streamFolders.constFind(root);
        if (it != streamFolders.cend())
            return *it;
    }
    QStringList folders;
    QDirIterator it(root, {QStringLiteral("Stream")}, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext() && folders.size() < kMaxStreamFolders)
        folders << it.next();
    const QMutexLocker lock(&streamMutex);
    streamFolders.insert(root, folders);
    return folders;
}

std::optional<std::pair<QByteArray, BwavInfo>> streamedCopy(const QString &fileName, const BwavInfo &stub)
{
    const QFileInfo named(fileName);
    const QString base = named.fileName();
    if (!base.endsWith(QLatin1String(".bwav"), Qt::CaseInsensitive))
        return std::nullopt;
    QStringList folders;
    if (named.isAbsolute())
        folders << named.dir().filePath(QStringLiteral("Stream"));
    for (const QString &root : zstdDictionaryRoots())
        folders << streamFoldersUnder(root);
    for (const QString &folder : folders) {
        QFile file(QDir(folder).filePath(base));
        if (file.fileName() == named.absoluteFilePath() || file.size() > BwavReader::kMaxFileSize
            || !file.open(QIODevice::ReadOnly))
            continue;
        QByteArray data = file.readAll();
        std::optional<BwavInfo> info = BwavReader::parse(data);
        if (info && !info->prefetch && info->channels.size() == stub.channels.size()
            && info->channels.front().start < data.size() && info->channels.front().samples > 0)
            return std::make_pair(std::move(data), std::move(*info));
    }
    return std::nullopt;
}

class SoundTable : public SimpleTable
{
public:
    SoundTable(std::vector<Row> rows, QByteArray data, BwavInfo info)
        : SimpleTable(QObject::tr("Sound"), {{QObject::tr("property")}, {QObject::tr("value")}}, std::move(rows)),
          m_data(std::move(data)), m_info(std::move(info))
    {
    }

    bool hasAudio() const override { return true; }
    std::optional<Audio> decodeAudio(QString *error) const override
    {
        try {
            return BwavReader::decode(m_data, m_info, error);
        } catch (const std::bad_alloc &) {
            if (error)
                *error = QObject::tr("Not enough memory to decode this sound.");
            return std::nullopt;
        }
    }

private:
    QByteArray m_data;
    BwavInfo m_info;
};

}

qint64 BwavInfo::dataEnd() const
{
    qint64 end = kHeaderSize + qint64(channels.size()) * kChannelSize;
    for (const BwavChannel &c : channels)
        end = std::max(end, c.start + channelBytes(c));
    return end;
}

std::vector<qint16> decodeDspAdpcm(QByteArrayView data, qint64 samples, const std::array<qint16, 16> &coefficients,
                                   qint16 history1, qint16 history2)
{
    std::vector<qint16> out;
    const qint64 frames = std::min<qint64>((samples + kSamplesPerFrame - 1) / kSamplesPerFrame, data.size() / kBytesPerFrame);
    out.reserve(size_t(std::min(samples, frames * kSamplesPerFrame)));
    int h1 = history1, h2 = history2;
    for (qint64 f = 0; f < frames && qint64(out.size()) < samples; ++f) {
        const auto *frame = reinterpret_cast<const uchar *>(data.data()) + f * kBytesPerFrame;
        const int predictor = (frame[0] >> 4) & 7;
        const int scale = 1 << (frame[0] & 0x0F);
        const int c1 = coefficients[size_t(predictor * 2)];
        const int c2 = coefficients[size_t(predictor * 2 + 1)];
        for (int i = 0; i < kSamplesPerFrame && qint64(out.size()) < samples; ++i) {
            const int byte = frame[1 + i / 2];
            int nibble = i % 2 == 0 ? byte >> 4 : byte & 0x0F;
            if (nibble >= 8)
                nibble -= 16;
            const int value = std::clamp(((nibble * scale) * 2048 + c1 * h1 + c2 * h2 + 1024) >> 11, -32768, 32767);
            out.push_back(qint16(value));
            h2 = h1;
            h1 = value;
        }
    }
    return out;
}

std::optional<BwavInfo> BwavReader::parse(QByteArrayView data, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<BwavInfo> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (data.size() < kHeaderSize || !data.startsWith("BWAV"))
        return fail(QObject::tr("Not a BWAV sound (no \"BWAV\" at the start)."));
    if (uchar(data[4]) != 0xFF || uchar(data[5]) != 0xFE)
        return fail(QObject::tr("Big-endian BWAV files are not supported."));
    const auto u16 = [&](qsizetype at) { return qFromLittleEndian<quint16>(data.data() + at); };
    const auto s16 = [&](qsizetype at) { return qFromLittleEndian<qint16>(data.data() + at); };
    const auto u32 = [&](qsizetype at) { return qFromLittleEndian<quint32>(data.data() + at); };
    const auto s32 = [&](qsizetype at) { return qFromLittleEndian<qint32>(data.data() + at); };
    BwavInfo info;
    info.prefetch = u16(0x0C) != 0;
    const int channels = u16(0x0E);
    if (channels < 1 || channels > kMaxChannels)
        return fail(QObject::tr("Unexpected channel count %1.").arg(channels));
    if (data.size() < kHeaderSize + channels * kChannelSize)
        return fail(QObject::tr("The channel table runs past the end of the file."));
    for (int c = 0; c < channels; ++c) {
        const qsizetype at = kHeaderSize + c * kChannelSize;
        BwavChannel ch;
        ch.codec = u16(at);
        ch.sampleRate = int(u32(at + 0x04));
        ch.fullSamples = u32(at + 0x08);
        ch.samples = u32(at + 0x0C);
        for (int k = 0; k < 16; ++k)
            ch.coefficients[size_t(k)] = s16(at + 0x10 + k * 2);
        ch.start = u32(at + 0x34);
        ch.loopEnd = s32(at + 0x3C);
        ch.loopStart = s32(at + 0x40);
        ch.history1 = s16(at + 0x46);
        ch.history2 = s16(at + 0x48);
        if (ch.sampleRate <= 0 || ch.sampleRate > 384000)
            return fail(QObject::tr("Unexpected sample rate %1.").arg(ch.sampleRate));
        info.channels.push_back(ch);
    }
    return info;
}

std::optional<Audio> BwavReader::decode(QByteArrayView data, const BwavInfo &info, QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<Audio> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (info.channels.empty())
        return fail(QObject::tr("The sound has no channels."));
    std::vector<std::vector<qint16>> channels;
    for (const BwavChannel &c : info.channels) {
        if (c.codec != 0 && c.codec != 1 && c.codec != 2)
            return fail(QObject::tr("%1 sounds cannot be played yet.").arg(codecName(c.codec)));
        if (c.samples == 0 || c.start >= data.size())
            return fail(QObject::tr("This file only describes the sound: its samples are stored in another file "
                                    "(usually a .bwav with the same name in a Stream folder)."));
        if (c.start < 0)
            return fail(QObject::tr("The sound data starts outside the file."));
        std::vector<qint16> samples;
        if (c.codec == 2) {
            if (c.start > data.size() - 0x28)
                return fail(QObject::tr("The Opus data of the sound is cut off."));
            if (qFromLittleEndian<quint32>(data.data() + c.start) != 0x80000001u && info.prefetch)
                return fail(QObject::tr("This file only describes the sound: its samples are stored in another file "
                                        "(usually a .bwav with the same name in a Stream folder)."));
            const qint64 size = 0x28 + qint64(qFromLittleEndian<quint32>(data.data() + c.start + 0x24));
            if (size > data.size() - c.start)
                return fail(QObject::tr("The Opus data of the sound is cut off."));
            const std::unique_ptr<Document> opus =
                NintendoOpusReader::fromBytes(data.sliced(c.start, size).toByteArray(), error);
            if (!opus || opus->tables.empty())
                return std::nullopt;
            const std::optional<Audio> decoded = opus->tables.front()->decodeAudio(error);
            if (!decoded || decoded->channels < 1)
                return std::nullopt;
            const qint64 decodedFrames = decoded->frameCount();
            samples.resize(size_t(decodedFrames));
            const auto *in = reinterpret_cast<const qint16 *>(decoded->pcm.constData());
            for (qint64 f = 0; f < decodedFrames; ++f)
                samples[size_t(f)] = qFromLittleEndian(in[f * decoded->channels]);
            if (c.samples > 0 && qint64(samples.size()) > c.samples)
                samples.resize(size_t(c.samples));
            channels.push_back(std::move(samples));
            continue;
        }
        const QByteArrayView bytes = data.sliced(c.start, std::min<qint64>(channelBytes(c), data.size() - c.start));
        if (c.codec == 1) {
            samples = decodeDspAdpcm(bytes, c.samples, c.coefficients, c.history1, c.history2);
        } else {
            samples.resize(size_t(bytes.size() / 2));
            for (size_t i = 0; i < samples.size(); ++i)
                samples[i] = qFromLittleEndian<qint16>(bytes.data() + i * 2);
        }
        channels.push_back(std::move(samples));
    }
    size_t frames = channels.front().size();
    for (const auto &c : channels)
        frames = std::min(frames, c.size());
    if (frames == 0)
        return fail(QObject::tr("This file only describes the sound: its samples are stored in another file "
                                "(usually a .bwav with the same name in a Stream folder)."));
    const int outChannels = channels.size() == 1 ? 1 : 2;
    Audio audio;
    audio.sampleRate = info.channels.front().sampleRate;
    audio.channels = outChannels;
    audio.pcm.resize(qsizetype(frames) * outChannels * 2);
    auto *out = reinterpret_cast<qint16 *>(audio.pcm.data());
    for (size_t f = 0; f < frames; ++f) {
        if (outChannels == 1) {
            qToLittleEndian(channels[0][f], out + f);
            continue;
        }
        int left = 0, right = 0, leftCount = 0, rightCount = 0;
        for (size_t c = 0; c < channels.size(); ++c) {
            if (c % 2 == 0)
                left += channels[c][f], ++leftCount;
            else
                right += channels[c][f], ++rightCount;
        }
        qToLittleEndian(qint16(left / std::max(leftCount, 1)), out + f * 2);
        qToLittleEndian(qint16(right / std::max(rightCount, 1)), out + f * 2 + 1);
    }
    return audio;
}

int BwavReader::score(const QString &, QByteArrayView head) const
{
    return head.startsWith("BWAV") ? 100 : 0;
}

std::unique_ptr<Document> BwavReader::read(const QString &path, QString *error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return nullptr;
    }
    if (file.size() > kMaxFileSize) {
        if (error)
            *error = QObject::tr("File is larger than 512 MB.");
        return nullptr;
    }
    return readBytes(file.readAll(), path, error);
}

std::unique_ptr<Document> BwavReader::readBytes(const QByteArray &input, const QString &fileName, QString *error) const
{
    QByteArray data = input;
    std::optional<BwavInfo> info = parse(data, error);
    if (!info)
        return nullptr;
    bool streamed = false;
    const BwavChannel &head = info->channels.front();
    if (info->prefetch || head.start >= data.size() || head.samples == 0)
        if (auto copy = streamedCopy(fileName, *info)) {
            data = std::move(copy->first);
            info = std::move(copy->second);
            streamed = true;
        }
    const BwavChannel first = info->channels.front();
    const bool stored = first.start < data.size() && first.samples > 0;
    const QString length = duration(first.fullSamples, first.sampleRate);
    std::vector<SimpleTable::Row> rows = {
        {QObject::tr("length"), length},
        {QObject::tr("channels"), int(info->channels.size())},
        {QObject::tr("sample rate"), QObject::tr("%1 Hz").arg(first.sampleRate)},
        {QObject::tr("codec"), codecName(first.codec)},
        {QObject::tr("samples"), first.fullSamples},
        {QObject::tr("samples in this file"), stored ? first.samples : 0},
        {QObject::tr("loop"), first.loopEnd >= 0 ? QObject::tr("%1 – %2 (%3 – %4)")
                                                       .arg(duration(first.loopStart, first.sampleRate),
                                                            duration(first.loopEnd, first.sampleRate))
                                                       .arg(first.loopStart)
                                                       .arg(first.loopEnd)
                                                 : QObject::tr("none")},
    };
    auto table = std::make_shared<SoundTable>(std::move(rows), data, std::move(*info));
    if (!stored)
        table->setDescription(QObject::tr("%1, %2 Hz — only the description: the samples are in another file")
                                  .arg(length)
                                  .arg(first.sampleRate));
    else if (streamed)
        table->setDescription(QObject::tr("%1, %2 Hz, %3, samples from the Stream folder")
                                  .arg(length)
                                  .arg(first.sampleRate)
                                  .arg(codecName(first.codec)));
    else
        table->setDescription(QObject::tr("%1, %2 Hz, %3").arg(length).arg(first.sampleRate).arg(codecName(first.codec)));
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(table));
    return doc;
}
