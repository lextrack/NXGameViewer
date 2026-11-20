#include "textures/texturesearch.h"

#include "archives/decompress.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace {

constexpr int kMaxDepth = 3;
constexpr int kMaxDirectories = 400;
constexpr int kMaxSiblingArchives = 48;
constexpr int kMaxSharedHolders = 24;
constexpr size_t kMaxCachedDocuments = 32;

bool looksLikeTextureFolder(const QString &name)
{
    static const QStringList words = {"tex", "texture", "textures", "image", "images", "img"};
    static const QRegularExpression separators(QStringLiteral("[^A-Za-z0-9]+"));
    const QStringList parts = name.toLower().split(separators, Qt::SkipEmptyParts);
    return std::any_of(parts.begin(), parts.end(), [](const QString &part) { return words.contains(part); });
}

void collect(const QString &root, int depth, QStringList &likely, QStringList &other)
{
    if (likely.size() + other.size() >= kMaxDirectories || depth > kMaxDepth)
        return;
    const QFileInfoList children = QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &child : children) {
        if (likely.size() + other.size() >= kMaxDirectories)
            return;
        (looksLikeTextureFolder(child.fileName()) ? likely : other) << child.absoluteFilePath();
        collect(child.absoluteFilePath(), depth + 1, likely, other);
    }
}

QStringList searchFolders(const QString &modelPath)
{
    const QString modelDir = QFileInfo(modelPath).absolutePath();
    QStringList folders{modelDir};
    QDir ancestor(modelDir);
    for (int level = 0; level < 2 && ancestor.cdUp(); ++level) {
        QStringList likely, other;
        collect(ancestor.absolutePath(), 1, likely, other);
        for (const QString &f : likely + other)
            if (!folders.contains(f))
                folders << f;
    }
    return folders;
}

QString findIn(const QStringList &folders, const QString &textureName)
{
    if (textureName.isEmpty() || textureName.contains(u'/') || textureName.contains(u'\\'))
        return {};
    static const QStringList extensions = {"xtx", "png", "dds", "tga", "bmp", "jpg", "bntx", "bntx.zs"};
    for (const QString &folder : folders) {
        for (const QString &ext : extensions) {
            const QString candidate = folder + u'/' + textureName + u'.' + ext;
            if (QFileInfo::exists(candidate))
                return candidate;
        }
    }
    return {};
}

bool mayHoldTextures(const QByteArray &head)
{
    return detectCompression(head) != Compression::None || head.startsWith("SARC") || head.startsWith("BNTX")
        || head.startsWith("FRES");
}

std::shared_ptr<const Document> openThroughArchives(const FormatRegistry &registry, const QString &path)
{
    std::shared_ptr<const Document> doc = registry.open(path, nullptr, nullptr);
    for (int depth = 0; doc && depth < 4; ++depth) {
        std::shared_ptr<const Archive> archive;
        for (const auto &t : doc->tables) {
            if (t->hasModel())
                return doc;
            if (auto a = t->archive())
                archive = a;
        }
        if (!archive || archive->entries.size() != 1)
            break;
        doc = registry.openBytes(archive->entryData(0), archive->entries[0].name, nullptr, nullptr);
    }
    return doc;
}

void findInSiblingArchives(const QString &modelPath, const Model3D &model, const FormatRegistry &registry,
                           const std::vector<size_t> &wanted, std::vector<std::shared_ptr<const Image>> &images)
{
    const QFileInfo info(modelPath);
    const QString stem = info.baseName();
    if (stem.size() < 3 || !info.exists())
        return;
    const QDir dir = info.absoluteDir();
    const QStringList siblings = dir.entryList({stem + u'*'}, QDir::Files, QDir::Name);
    int opened = 0;
    for (const QString &name : siblings) {
        if (name == info.fileName())
            continue;
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::ReadOnly) || !mayHoldTextures(file.read(16)))
            continue;
        file.close();
        if (++opened > kMaxSiblingArchives)
            break;
        const std::shared_ptr<const Document> doc = openThroughArchives(registry, dir.filePath(name));
        if (!doc)
            continue;
        bool done = true;
        for (size_t i : wanted) {
            if (images[i])
                continue;
            const QString &texture = model.textures[i].name;
            for (const auto &table : doc->tables) {
                const QString tableName = table->name();
                if (table->hasImage() && !table->hasModel()
                    && (tableName == texture || tableName.endsWith(QStringLiteral(" / ") + texture))) {
                    images[i] = table->image();
                    break;
                }
            }
            done = done && images[i];
        }
        if (done)
            return;
    }
}

QString textureStem(const QString &name)
{
    return name.section(u'/', -1).section(u'\\', -1).section(u'.', 0, 0).toLower();
}

std::shared_ptr<const Archive> archiveIn(const Document &doc)
{
    for (const auto &t : doc.tables)
        if (auto a = t->archive())
            return a;
    return nullptr;
}

bool holdsTextures(const QString &entryName)
{
    static const QStringList extensions = {"wtd", "wtx", "dds", "xtx", "bntx", "png", "tga"};
    return extensions.contains(QFileInfo(entryName).suffix().toLower());
}

bool sharedHolder(const QString &entryName)
{
    const QString stem = textureStem(entryName);
    return stem.contains(QStringLiteral("texture")) || stem.contains(QStringLiteral("txd"));
}

std::shared_ptr<const Document> cachedDocument(TextureFolderCache *cache, const QString &key,
                                               const std::function<std::shared_ptr<const Document>()> &open)
{
    return cache ? cache->document(key, open) : open();
}

bool takeFrom(const Document &doc, const Model3D &model, const std::vector<size_t> &wanted,
              std::vector<std::shared_ptr<const Image>> &images)
{
    bool done = true;
    for (size_t i : wanted) {
        if (images[i])
            continue;
        const QString stem = textureStem(model.textures[i].name);
        for (const auto &table : doc.tables) {
            if (table->hasImage() && !table->hasModel() && textureStem(table->name()) == stem) {
                images[i] = table->image();
                break;
            }
        }
        done = done && images[i];
    }
    return done;
}

bool findInArchive(const std::shared_ptr<const Archive> &archive, const QString &key, const Model3D &model,
                   const FormatRegistry &registry, const std::vector<size_t> &wanted,
                   std::vector<std::shared_ptr<const Image>> &images, TextureFolderCache *cache)
{
    QSet<QString> stems;
    for (size_t i : wanted)
        stems.insert(textureStem(model.textures[i].name));
    std::vector<size_t> holders;
    for (size_t e = 0; e < archive->entries.size(); ++e) {
        const QString &name = archive->entries[e].name;
        if (holdsTextures(name) && (stems.contains(textureStem(name)) || sharedHolder(name)))
            holders.push_back(e);
    }
    std::stable_sort(holders.begin(), holders.end(), [&](size_t a, size_t b) {
        return stems.contains(textureStem(archive->entries[a].name)) > stems.contains(textureStem(archive->entries[b].name));
    });
    int opened = 0;
    for (size_t e : holders) {
        if (++opened > kMaxSharedHolders)
            break;
        const std::shared_ptr<const Document> doc =
            cachedDocument(cache, key + u'\n' + archive->entries[e].name, [&]() -> std::shared_ptr<const Document> {
                return registry.openEntry(*archive, e, nullptr, nullptr);
            });
        if (doc && takeFrom(*doc, model, wanted, images))
            return true;
    }
    return false;
}

void findInSharedArchives(const QString &location, const std::shared_ptr<const Archive> &container,
                          const Model3D &model, const FormatRegistry &registry, const std::vector<size_t> &wanted,
                          std::vector<std::shared_ptr<const Image>> &images, TextureFolderCache *cache)
{
    if (container
        && findInArchive(container, QStringLiteral("container:%1:%2:%3")
                                .arg(quintptr(container.get()))
                                .arg(container->entries.size())
                                .arg(container->entries.empty() ? QString() : container->entries.front().name), model, registry,
                         wanted, images, cache))
        return;
    const QFileInfo info(location);
    if (location.isEmpty() || !info.isFile())
        return;
    QString family = info.completeBaseName();
    while (!family.isEmpty() && family.back().isDigit())
        family.chop(1);
    if (family.size() < 3)
        return;
    const QDir dir = info.absoluteDir();
    QStringList siblings = dir.entryList({family + QStringLiteral(".") + info.suffix()}, QDir::Files);
    siblings.removeAll(info.fileName());
    for (const QString &name : siblings) {
        const QString path = dir.filePath(name);
        const std::shared_ptr<const Document> doc = cachedDocument(cache, QStringLiteral("file:") + path, [&] {
            return std::shared_ptr<const Document>(registry.open(path, nullptr, nullptr));
        });
        const std::shared_ptr<const Archive> archive = doc ? archiveIn(*doc) : nullptr;
        if (archive && findInArchive(archive, QStringLiteral("file:") + path, model, registry, wanted, images, cache))
            return;
    }
}

}

std::shared_ptr<const Document> TextureFolderCache::document(const QString &key,
                                                             const std::function<std::shared_ptr<const Document>()> &open)
{
    {
        const std::lock_guard lock(m_mutex);
        if (const auto it = m_documents.find(key); it != m_documents.end())
            return it->second;
    }
    std::shared_ptr<const Document> doc = open();
    const std::lock_guard lock(m_mutex);
    if (m_documents.size() >= kMaxCachedDocuments)
        m_documents.erase(m_documents.begin());
    return m_documents.emplace(key, std::move(doc)).first->second;
}

QStringList TextureFolderCache::folders(const QString &modelPath)
{
    const QString key = QFileInfo(modelPath).absolutePath();
    {
        const std::lock_guard lock(m_mutex);
        if (const auto it = m_folders.find(key); it != m_folders.end())
            return it->second;
    }
    QStringList found = searchFolders(modelPath);
    const std::lock_guard lock(m_mutex);
    return m_folders.emplace(key, std::move(found)).first->second;
}

QString findTextureFile(const QString &modelPath, const QString &textureName)
{
    if (modelPath.isEmpty())
        return {};
    return findIn(searchFolders(modelPath), textureName);
}

std::vector<std::shared_ptr<const Image>> loadModelTextures(const Model3D &model, const FormatRegistry &registry,
                                                           QStringList *missing, TextureFolderCache *cache,
                                                           const QString &location,
                                                           const std::shared_ptr<const Archive> &container)
{
    const QString base = location.isEmpty() ? model.sourcePath : location;
    std::vector<bool> used(model.textures.size(), false);
    for (const ModelMaterial &m : model.materials) {
        if (m.baseColorTexture >= 0 && size_t(m.baseColorTexture) < used.size())
            used[size_t(m.baseColorTexture)] = true;
        if (m.normalTexture >= 0 && size_t(m.normalTexture) < used.size())
            used[size_t(m.normalTexture)] = true;
    }
    std::vector<std::shared_ptr<const Image>> images(model.textures.size());
    const QStringList folders = base.isEmpty() ? QStringList()
                              : cache        ? cache->folders(base)
                                             : searchFolders(base);
    for (size_t i = 0; i < model.textures.size(); ++i) {
        if (!used[i])
            continue;
        if (model.embeddedTexture) {
            images[i] = model.embeddedTexture(model.textures[i].name);
            if (images[i])
                continue;
        }
        const QString path = findIn(folders, model.textures[i].name);
        std::shared_ptr<const Document> doc = path.isEmpty() ? nullptr : registry.open(path, nullptr, nullptr);
        if (doc) {
            for (const auto &table : doc->tables) {
                if (table->hasImage()) {
                    images[i] = table->image();
                    break;
                }
            }
        }
    }
    std::vector<size_t> unresolved;
    for (size_t i = 0; i < images.size(); ++i)
        if (used[i] && !images[i])
            unresolved.push_back(i);
    if (!unresolved.empty() && !base.isEmpty())
        findInSiblingArchives(base, model, registry, unresolved, images);
    if (std::any_of(unresolved.begin(), unresolved.end(), [&images](size_t i) { return !images[i]; }))
        findInSharedArchives(base, container, model, registry, unresolved, images, cache);
    if (missing)
        for (size_t i : unresolved)
            if (!images[i])
                *missing << model.textures[i].name;
    return images;
}
