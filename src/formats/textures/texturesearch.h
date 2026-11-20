#pragma once

#include "models/model3d.h"
#include "core/registry.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <map>
#include <memory>
#include <mutex>

class TextureFolderCache
{
public:
    QStringList folders(const QString &modelPath);
    std::shared_ptr<const Document> document(const QString &key, const std::function<std::shared_ptr<const Document>()> &open);

private:
    std::mutex m_mutex;
    std::map<QString, QStringList> m_folders;
    std::map<QString, std::shared_ptr<const Document>> m_documents;
};

QString findTextureFile(const QString &modelPath, const QString &textureName);

std::vector<std::shared_ptr<const Image>> loadModelTextures(const Model3D &model, const FormatRegistry &registry,
                                                           QStringList *missing = nullptr,
                                                           TextureFolderCache *cache = nullptr,
                                                           const QString &location = {},
                                                           const std::shared_ptr<const Archive> &container = {});
