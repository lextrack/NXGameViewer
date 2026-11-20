#include "modding/nestededit.h"

#include "archives/sarcwriter.h"

#include <QObject>

#include <new>

namespace {

constexpr int kMaxDepth = 8;

std::optional<QByteArray> editLevel(const QByteArray &stored, const QString &name, const QStringList &entries, int depth,
                                    const LeafEdit &edit, std::vector<RebuiltLayer> *rebuilt, QString *error)
{
    if (depth > kMaxDepth) {
        if (error)
            *error = QObject::tr("The archives are nested too deeply.");
        return std::nullopt;
    }
    std::vector<CompressionLayer> layers;
    const std::optional<QByteArray> plain = unpackAll(stored, &layers, error);
    if (!plain)
        return std::nullopt;

    std::optional<QByteArray> changed;
    if (entries.isEmpty()) {
        changed = edit(*plain, error);
    } else {
        if (!plain->startsWith("SARC")) {
            if (error)
                *error = QObject::tr("%1 is not a SARC archive; only files inside SARC archives can be replaced.").arg(name);
            return std::nullopt;
        }
        const std::optional<SarcContents> contents = readSarc(*plain, error);
        if (!contents)
            return std::nullopt;
        const QString &child = entries.first();
        const SarcEntry *entry = nullptr;
        for (const SarcEntry &e : contents->entries)
            if (!entry && sarcEntryName(e) == child)
                entry = &e;
        if (!entry) {
            if (error)
                *error = QObject::tr("%1 is not in %2.").arg(child, name);
            return std::nullopt;
        }
        const std::optional<QByteArray> newChild =
            editLevel(entry->data, child, entries.mid(1), depth + 1, edit, rebuilt, error);
        if (!newChild)
            return std::nullopt;
        changed = replaceSarcEntry(*plain, child, *newChild, error);
    }
    if (!changed)
        return std::nullopt;

    std::optional<QByteArray> packed = layers.empty() ? changed : packAll(*changed, layers, error);
    if (!packed)
        return std::nullopt;
    if (rebuilt && !entries.isEmpty()) {
        RebuiltLayer layer;
        layer.name = name;
        for (const CompressionLayer &l : layers)
            layer.compression << compressionName(l.kind);
        layer.oldUnpacked = plain->size();
        layer.newUnpacked = changed->size();
        layer.oldStored = stored.size();
        layer.newStored = packed->size();
        rebuilt->insert(rebuilt->begin(), layer);
    }
    return packed;
}

}

std::optional<QByteArray> editNested(const QByteArray &file, const QString &fileName, const QStringList &entries,
                                     const LeafEdit &edit, std::vector<RebuiltLayer> *rebuilt, QString *error)
{
    try {
        return editLevel(file, fileName, entries, 0, edit, rebuilt, error);
    } catch (const std::bad_alloc &) {
        if (error)
            *error = QObject::tr("Not enough memory to rebuild this file.");
        return std::nullopt;
    }
}
