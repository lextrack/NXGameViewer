#include "models/animationsearch.h"

#include "models/bfresreader.h"
#include "archives/decompress.h"
#include "models/falcommdlreader.h"
#include "archives/sarcreader.h"
#include "rage/rageanimation.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <map>

namespace {

constexpr qint64 kMaxAnimationFileSize = qint64{64} * 1024 * 1024;
constexpr int kMaxCandidates = 256;

QByteArray readSmallFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return {};
    }
    if (file.size() > kMaxAnimationFileSize) {
        if (error)
            *error = QObject::tr("File is larger than 64 MB.");
        return {};
    }
    return file.readAll();
}

struct Unpacked
{
    QString entry;
    QByteArray data;
};

std::vector<Unpacked> unpackBfres(QByteArray data, QString *error)
{
    for (int depth = 0; depth < 4 && detectCompression(data) != Compression::None; ++depth) {
        std::optional<QByteArray> unpacked = decompress(data, error);
        if (!unpacked)
            return {};
        data = std::move(*unpacked);
    }
    if (BfresReader::isBfres(data))
        return {{QString(), data}};
    std::vector<Unpacked> found;
    if (data.startsWith("SARC")) {
        const std::unique_ptr<Document> doc = SarcReader().readBytes(data, {}, error);
        if (!doc)
            return {};
        for (const auto &table : doc->tables)
            if (const auto archive = table->archive())
                for (size_t i = 0; i < archive->entries.size(); ++i) {
                    QByteArray entry = archive->entryData(i);
                    if (BfresReader::isBfres(entry))
                        found.push_back({archive->entries[i].name, std::move(entry)});
                }
    }
    return found;
}

bool mayHoldAnimations(const QByteArray &head)
{
    return detectCompression(head) != Compression::None || head.startsWith("SARC") || BfresReader::isBfres(head)
        || head.startsWith("MDL ") || isRageAnimationSet(head);
}

int countMatches(const QStringList &bones, const QSet<QString> &nodes)
{
    int matched = 0;
    for (const QString &bone : bones)
        matched += nodes.contains(bone) ? 1 : 0;
    return matched;
}

bool fits(int matched, qsizetype bones)
{
    return matched > 0 && matched * 2 >= bones;
}

}

std::vector<AnimationFile> inspectAnimationFile(const QString &path, const Model3D &model)
{
    const QByteArray data = readSmallFile(path, nullptr);
    if (data.isEmpty())
        return {};
    QSet<QString> nodes;
    for (const ModelNode &n : model.nodes)
        nodes.insert(n.name);
    std::vector<AnimationFile> found;
    if (isRageAnimationSet(data)) {
        QSet<int> ids;
        for (const ModelNode &n : model.nodes)
            if (n.boneId >= 0)
                ids.insert(n.boneId);
        if (ids.isEmpty())
            return found;
        const QString stem = QFileInfo(path).completeBaseName();
        for (const RageClipInfo &clip : rageAnimationClips(data, nullptr).value_or(std::vector<RageClipInfo>{})) {
            int matched = 0;
            for (int id : clip.boneIds)
                matched += ids.contains(id) ? 1 : 0;
            if (!fits(matched, qsizetype(clip.boneIds.size())))
                continue;
            AnimationFile file;
            file.path = path;
            file.name = QStringLiteral("%1 — %2").arg(clip.name.section(u'/', -1), stem);
            file.matchedBones = matched;
            file.bones = int(clip.boneIds.size());
            file.clip = clip.name;
            file.frames = clip.frames;
            found.push_back(std::move(file));
        }
        return found;
    }
    if (data.startsWith("MDL ")) {
        const QStringList bones = FalcomMdlReader::animatedNodeNames(data);
        const int matched = countMatches(bones, nodes);
        if (fits(matched, bones.size()))
            found.push_back({path, QFileInfo(path).completeBaseName(), matched, int(bones.size())});
        return found;
    }
    const QString stem = QFileInfo(path).baseName();
    for (const Unpacked &bfres : unpackBfres(data, nullptr)) {
        for (const BfresClip &clip : BfresReader::skeletalClips(bfres.data)) {
            const QStringList &bones = clip.animatedBones.isEmpty() ? clip.bones : clip.animatedBones;
            const int matched = countMatches(bones, nodes);
            if (!fits(matched, bones.size()) || (bones.size() >= 2 && matched < 2))
                continue;
            AnimationFile file;
            file.path = path;
            file.name = QStringLiteral("%1 — %2").arg(clip.name, stem);
            file.matchedBones = matched;
            file.bones = int(bones.size());
            file.clip = clip.name;
            file.entry = bfres.entry;
            file.frames = clip.frames;
            found.push_back(std::move(file));
        }
    }
    return found;
}

std::vector<AnimationFile> findAnimationFiles(const QString &modelPath, const Model3D &model)
{
    const QFileInfo info(modelPath);
    if (!info.exists())
        return {};
    const QDir dir = info.absoluteDir();
    QStringList candidates;
    const QString stem = info.completeBaseName().section(u'_', 0, 0);
    if (info.suffix().compare(QStringLiteral("mdl"), Qt::CaseInsensitive) == 0) {
        if (!stem.isEmpty())
            candidates = dir.entryList({stem + QStringLiteral("_*.mdl")}, QDir::Files, QDir::Name);
    } else {
        candidates << info.fileName();
        for (const QString &name :
             dir.entryList({QStringLiteral("*anim*"), QStringLiteral("*motion*"), QStringLiteral("*.was")}, QDir::Files,
                           QDir::Name))
            if (!candidates.contains(name))
                candidates << name;
    }
    std::vector<AnimationFile> found;
    int inspected = 0;
    for (const QString &name : candidates) {
        const bool self = name == info.fileName();
        if (self && info.suffix().compare(QStringLiteral("mdl"), Qt::CaseInsensitive) == 0)
            continue;
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::ReadOnly) || !mayHoldAnimations(file.read(16)))
            continue;
        file.close();
        if (++inspected > kMaxCandidates)
            break;
        for (AnimationFile &a : inspectAnimationFile(dir.filePath(name), model))
            found.push_back(std::move(a));
    }
    const QString modelName = info.baseName();
    auto closeness = [&modelName](const AnimationFile &a) {
        const QString other = QFileInfo(a.path).baseName();
        qsizetype n = 0;
        while (n < modelName.size() && n < other.size() && modelName[n] == other[n])
            ++n;
        return n;
    };
    std::stable_sort(found.begin(), found.end(), [&](const AnimationFile &a, const AnimationFile &b) {
        const qsizetype ca = closeness(a), cb = closeness(b);
        if (ca != cb)
            return ca > cb;
        return qint64(a.matchedBones) * b.bones > qint64(b.matchedBones) * a.bones;
    });
    return found;
}

std::vector<ModelAnimation> loadAnimations(const std::vector<AnimationFile> &animations, const Model3D &model,
                                           QStringList *problems)
{
    std::vector<ModelAnimation> out;
    std::map<QString, QByteArray> files;
    std::map<std::pair<QString, QString>, QByteArray> unpacked;
    for (const AnimationFile &a : animations) {
        const QString label = a.clip.isEmpty() ? QFileInfo(a.path).fileName()
                                               : QStringLiteral("%1 (%2)").arg(a.clip, QFileInfo(a.path).fileName());
        QString error;
        auto file = files.find(a.path);
        if (file == files.end())
            file = files.emplace(a.path, readSmallFile(a.path, &error)).first;
        std::optional<ModelAnimation> animation;
        if (error.isEmpty() && file->second.isEmpty())
            error = QObject::tr("The file is empty.");
        if (error.isEmpty()) {
            if (isRageAnimationSet(file->second)) {
                animation = readRageAnimationClip(file->second, a.clip, model, &error);
            } else if (a.clip.isEmpty()) {
                animation = FalcomMdlReader::readAnimation(file->second, model, QFileInfo(a.path).completeBaseName(), &error);
            } else {
                const auto key = std::make_pair(a.path, a.entry);
                auto bfres = unpacked.find(key);
                if (bfres == unpacked.end()) {
                    QByteArray found;
                    for (Unpacked &u : unpackBfres(file->second, &error))
                        if (u.entry == a.entry)
                            found = std::move(u.data);
                    bfres = unpacked.emplace(key, std::move(found)).first;
                }
                if (bfres->second.isEmpty()) {
                    if (error.isEmpty())
                        error = QObject::tr("No BFRES file was found inside.");
                } else {
                    animation = BfresReader::readSkeletalClip(bfres->second, a.clip, model, &error);
                }
            }
        }
        if (animation)
            out.push_back(std::move(*animation));
        else if (problems)
            *problems << QStringLiteral("%1: %2").arg(label, error);
    }
    return out;
}
