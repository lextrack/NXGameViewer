#include "rage/rageanimation.h"

#include "core/simpletable.h"
#include "rage/rageresource.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QObject>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace {

constexpr int kMaxClips = 1 << 16;
constexpr int kMaxBlocks = 1 << 12;
constexpr int kMaxTracks = 1 << 12;
constexpr int kMaxFrames = 1 << 16;
constexpr int kTranslation = 0;
constexpr int kRotation = 1;
constexpr int kScale = 2;

using Vec4 = std::array<float, 4>;

struct Clip
{
    QString name;
    qint64 animation = -1;
    float start = 0;
    float end = 0;
};

struct Track
{
    int id = 0;
    int format = 0;
    int bone = 0;
};

class Animation
{
public:
    Animation(const RageView &v, qint64 at) : m_v(v)
    {
        m_frames = v.u16(at + 8);
        m_framesPerChunk = v.u16(at + 0x0A);
        m_seconds = v.f32(at + 0x0C);
        const int blockCount = v.u16(at + 0x18);
        if (m_frames == 0 || m_frames > kMaxFrames || m_framesPerChunk == 0 || blockCount == 0 || blockCount > kMaxBlocks
            || !std::isfinite(m_seconds) || m_seconds < 0)
            throw RageDamaged();
        const qint64 blocks = v.at(v.u32(at + 0x14));
        for (int b = 0; b < blockCount; ++b) {
            const qint64 block = v.at(v.u32(blocks + 4 * b));
            const int chunkCount = v.u16(block + 8);
            if (chunkCount > kMaxTracks)
                throw RageDamaged();
            const qint64 chunks = chunkCount ? v.at(v.u32(block + 4)) : 0;
            std::vector<qint64> list;
            for (int c = 0; c < chunkCount; ++c)
                list.push_back(v.at(v.u32(chunks + 4 * c)));
            m_blocks.push_back(std::move(list));
        }
        for (qint64 chunk : m_blocks.front())
            m_tracks.push_back({v.u8(chunk), v.u8(chunk + 1), v.u16(chunk + 2)});
    }

    int frames() const { return m_frames; }
    float seconds() const { return m_seconds; }
    const std::vector<Track> &tracks() const { return m_tracks; }

    Vec4 evaluate(size_t track, int frame) const
    {
        const size_t block = std::min(size_t(frame / m_framesPerChunk), m_blocks.size() - 1);
        const int local = frame - int(block) * m_framesPerChunk;
        if (track >= m_blocks[block].size())
            throw RageDamaged();
        const qint64 chunk = m_blocks[block][track];
        Vec4 v{0, 0, 0, 0};
        int filled = 0;
        for (int k = 0; k < 4 && filled < 4; ++k) {
            const std::optional<qint64> channel = m_v.maybe(m_v.u32(chunk + 4 + 4 * k));
            if (channel)
                filled += channelValue(*channel, local, v, filled);
        }
        return v;
    }

private:
    quint32 packed(qint64 at, int index) const
    {
        const qint64 data = m_v.at(m_v.u32(at));
        const quint32 bits = m_v.u32(at + 4);
        const quint32 count = m_v.u32(at + 8);
        if (bits == 0 || count == 0)
            return 0;
        if (bits > 32)
            throw RageDamaged();
        const quint64 bit = quint64(std::clamp<qint64>(index, 0, qint64(count) - 1)) * bits;
        const qint64 word = qint64(bit >> 5);
        quint64 value = m_v.u32(data + 4 * word);
        m_v.need(data, 4 * (word + 1));
        if (((bit & 31) + bits) > 32)
            value |= quint64(m_v.u32(data + 4 * (word + 1))) << 32;
        const quint64 mask = bits == 32 ? 0xFFFFFFFFull : ((quint64(1) << bits) - 1);
        return quint32((value >> (bit & 31)) & mask);
    }

    int channelValue(qint64 at, int frame, Vec4 &v, int slot) const
    {
        auto put = [&](float value) {
            if (slot < 4)
                v[size_t(slot)] = value;
            return 1;
        };
        switch (m_v.u8(at + 5)) {
        case 1: {
            const int count = m_v.u16(at + 0x0C);
            if (count == 0)
                return put(0);
            return put(m_v.f32(m_v.at(m_v.u32(at + 8)) + 4 * std::clamp(frame, 0, count - 1)));
        }
        case 4: return put(m_v.f32(at + 8));
        case 6: return put(float(packed(at + 8, frame)) * m_v.f32(at + 0x14) + m_v.f32(at + 0x18));
        case 9: {
            const qint64 q = m_v.at(m_v.u32(at + 8));
            for (int i = 0; i < 4; ++i)
                if (slot + i < 4)
                    v[size_t(slot + i)] = m_v.f32(q + 4 * i);
            return 4;
        }
        case 13: {
            const qint64 p = m_v.at(m_v.u32(at + 8));
            for (int i = 0; i < 3; ++i)
                if (slot + i < 4)
                    v[size_t(slot + i)] = m_v.f32(p + 4 * i);
            return 3;
        }
        case 14: {
            std::array<float, 3> part{};
            for (int i = 0; i < 3; ++i) {
                const qint64 entry = at + 8 + 8 * i;
                part[size_t(i)] = m_v.f32(entry);
                if (const std::optional<qint64> scaled = m_v.maybe(m_v.u32(entry + 4)))
                    part[size_t(i)] += float(packed(*scaled + 4, frame)) * m_v.f32(*scaled);
            }
            const float w = std::sqrt(std::max(0.0f, 1.0f - (part[0] * part[0] + part[1] * part[1] + part[2] * part[2])));
            const quint32 order = std::min<quint32>(m_v.u32(at + 0x20), 3);
            Vec4 q{};
            int next = 0;
            for (int i = 0; i < 4; ++i)
                q[size_t(i)] = quint32(i) == order ? w : part[size_t(next++)];
            if (slot == 0)
                v = q;
            return 4;
        }
        default: throw RageDamaged();
        }
    }

    const RageView &m_v;
    int m_frames = 0;
    int m_framesPerChunk = 1;
    float m_seconds = 0;
    std::vector<std::vector<qint64>> m_blocks;
    std::vector<Track> m_tracks;
};

std::optional<RageResource> animationResource(const QByteArray &data, QString *error)
{
    std::optional<RageResource> r = readRageResource(data, error);
    if (r && r->type != RageAnimationReader::kType) {
        if (error)
            *error = QObject::tr("This RAGE resource (type %1) is not an animation set.").arg(r->type);
        return std::nullopt;
    }
    return r;
}

std::vector<Clip> clips(const RageView &v)
{
    const qint64 dictionary = v.at(v.u32(v.objectStart() + 0x14));
    const int slotCount = v.u16(dictionary + 0x18);
    const qint64 slotList = slotCount ? v.at(v.u32(dictionary + 0x14)) : 0;
    std::vector<Clip> out;
    QSet<qint64> seen;
    for (int s = 0; s < slotCount; ++s) {
        for (quint32 entry = v.u32(slotList + 4 * s); entry != 0;) {
            const qint64 at = v.at(entry);
            if (seen.contains(at) || int(seen.size()) >= kMaxClips)
                throw RageDamaged();
            seen.insert(at);
            entry = v.u32(at + 8);
            const std::optional<qint64> clip = v.maybe(v.u32(at + 4));
            if (!clip || v.u32(*clip + 4) != 1)
                continue;
            Clip c;
            c.name = v.cstring(v.u32(*clip + 8)).replace(u'\\', u'/');
            if (c.name.isEmpty())
                c.name = QStringLiteral("clip %1").arg(v.u32(at), 8, 16, QLatin1Char('0'));
            const std::optional<qint64> animation = v.maybe(v.u32(*clip + 0x28));
            if (!animation)
                continue;
            c.animation = *animation;
            c.start = v.f32(*clip + 0x2C);
            c.end = v.f32(*clip + 0x30);
            out.push_back(std::move(c));
        }
    }
    std::sort(out.begin(), out.end(), [](const Clip &a, const Clip &b) { return a.name < b.name; });
    return out;
}

std::pair<int, int> window(const Clip &clip, const Animation &animation)
{
    const int last = animation.frames() - 1;
    if (last <= 0 || animation.seconds() <= 0 || !std::isfinite(clip.start) || !std::isfinite(clip.end)
        || clip.end <= clip.start)
        return {0, last};
    const float step = animation.seconds() / float(last);
    const int first = std::clamp(int(std::lround(clip.start / step)), 0, last);
    const int end = std::clamp(int(std::lround(clip.end / step)), first, last);
    return {first, end};
}

bool animatesBone(const Track &t)
{
    return t.id == kTranslation || t.id == kRotation || t.id == kScale;
}

}

bool isRageAnimationSet(QByteArrayView head)
{
    return isRageResource(head) && qFromLittleEndian<quint32>(head.data() + 4) == RageAnimationReader::kType;
}

std::optional<std::vector<RageClipInfo>> rageAnimationClips(const QByteArray &data, QString *error)
{
    std::optional<RageResource> r = animationResource(data, error);
    if (!r)
        return std::nullopt;
    const RageView v(*r);
    std::vector<RageClipInfo> out;
    try {
        for (const Clip &c : clips(v)) {
            try {
                const Animation animation(v, c.animation);
                const auto [first, last] = window(c, animation);
                RageClipInfo info;
                info.name = c.name;
                info.frames = last - first + 1;
                info.seconds = animation.frames() > 1 ? animation.seconds() * float(last - first) / float(animation.frames() - 1)
                                                      : 0.0f;
                info.tracks = int(animation.tracks().size());
                QSet<int> bones;
                for (const Track &t : animation.tracks())
                    if (animatesBone(t))
                        bones.insert(t.bone);
                info.boneIds.assign(bones.begin(), bones.end());
                std::sort(info.boneIds.begin(), info.boneIds.end());
                out.push_back(std::move(info));
            } catch (const RageDamaged &) {
            }
        }
    } catch (const RageDamaged &) {
        if (error)
            *error = QObject::tr("The animation set is damaged.");
        return std::nullopt;
    }
    return out;
}

std::optional<ModelAnimation> readRageAnimationClip(const QByteArray &data, const QString &clip, const Model3D &model,
                                                    QString *error)
{
    auto fail = [error](const QString &message) -> std::optional<ModelAnimation> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    std::optional<RageResource> r = animationResource(data, error);
    if (!r)
        return std::nullopt;
    QHash<int, int> nodeOf;
    for (size_t i = 0; i < model.nodes.size(); ++i)
        if (model.nodes[i].boneId >= 0 && !nodeOf.contains(model.nodes[i].boneId))
            nodeOf.insert(model.nodes[i].boneId, int(i));
    if (nodeOf.isEmpty())
        return fail(QObject::tr("The model has no RAGE skeleton to animate."));
    const RageView v(*r);
    try {
        for (const Clip &c : clips(v)) {
            if (c.name != clip)
                continue;
            const Animation animation(v, c.animation);
            const auto [first, last] = window(c, animation);
            const float step = animation.frames() > 1 ? animation.seconds() / float(animation.frames() - 1) : 0.0f;
            ModelAnimation out;
            out.name = c.name.section(u'/', -1);
            std::vector<float> times;
            for (int f = first; f <= last; ++f)
                times.push_back(float(f - first) * step);
            for (size_t t = 0; t < animation.tracks().size(); ++t) {
                const Track &track = animation.tracks()[t];
                const auto node = nodeOf.constFind(track.bone);
                if (!animatesBone(track) || node == nodeOf.cend())
                    continue;
                AnimationChannel channel;
                channel.node = *node;
                channel.path = track.id == kRotation ? AnimationPath::Rotation
                             : track.id == kScale    ? AnimationPath::Scale
                                                     : AnimationPath::Translation;
                channel.times = times;
                Vec4 previous{0, 0, 0, 1};
                for (int f = first; f <= last; ++f) {
                    Vec4 value = animation.evaluate(t, f);
                    if (track.id == kRotation) {
                        const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]
                                                       + value[3] * value[3]);
                        if (length < 1e-6f)
                            value = {0, 0, 0, 1};
                        else
                            for (float &x : value)
                                x /= length;
                        if (value[0] * previous[0] + value[1] * previous[1] + value[2] * previous[2] + value[3] * previous[3] < 0)
                            for (float &x : value)
                                x = -x;
                        previous = value;
                        channel.values.insert(channel.values.end(), value.begin(), value.end());
                    } else {
                        channel.values.insert(channel.values.end(), value.begin(), value.begin() + 3);
                    }
                }
                out.channels.push_back(std::move(channel));
            }
            if (out.channels.empty())
                return fail(QObject::tr("None of the clip's bones are in this model."));
            return out;
        }
    } catch (const RageDamaged &) {
        return fail(QObject::tr("The animation set is damaged."));
    }
    return fail(QObject::tr("The clip %1 is not in this animation set.").arg(clip));
}

int RageAnimationReader::score(const QString &, QByteArrayView head) const
{
    return isRageAnimationSet(head) ? 60 : 0;
}

std::unique_ptr<Document> RageAnimationReader::read(const QString &path, QString *error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return nullptr;
    }
    if (file.size() > kMaxFileSize) {
        if (error)
            *error = QObject::tr("File is larger than 256 MB.");
        return nullptr;
    }
    return readBytes(file.readAll(), path, error);
}

std::unique_ptr<Document> RageAnimationReader::readBytes(const QByteArray &data, const QString &fileName, QString *error) const
{
    std::optional<std::vector<RageClipInfo>> list = rageAnimationClips(data, error);
    if (!list)
        return nullptr;
    if (list->empty()) {
        if (error)
            *error = QObject::tr("This animation set has no clips the app can read.");
        return nullptr;
    }
    std::vector<SimpleTable::Row> rows;
    for (const RageClipInfo &c : *list)
        rows.push_back({c.name, c.frames, QString::number(double(c.seconds), 'f', 2), qint64(c.boneIds.size()), c.tracks});
    const QString base = QFileInfo(fileName).completeBaseName();
    auto table = std::make_shared<SimpleTable>(
        base.isEmpty() ? QObject::tr("Clips") : base,
        std::vector<SimpleColumn>{{QObject::tr("clip")}, {QObject::tr("frames")}, {QObject::tr("seconds")},
                                  {QObject::tr("bones")}, {QObject::tr("tracks")}},
        std::move(rows));
    table->setDescription(QObject::tr("RAGE animation set, %n clip(s). Export a model with animations to use them.", nullptr,
                                      int(list->size())));
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(table));
    return doc;
}
