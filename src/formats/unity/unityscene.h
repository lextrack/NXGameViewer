#pragma once

#include "models/model3d.h"
#include "unity/serializedfile.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <map>
#include <memory>
#include <optional>
#include <vector>

struct UnitySceneObject
{
    int file = 0;
    qint64 pathId = 0;
    int classId = 0;
    UValue value;
    QByteArray resource;
};

struct UnityScene
{
    std::vector<UnitySceneObject> objects;
    std::vector<QList<int>> externals;
    QString unityVersion;
    bool bigEndian = false;
    qint32 platform = 0;

    std::map<std::pair<int, qint64>, size_t> index;

    void buildIndex();
    const UnitySceneObject *find(int file, qint64 pathId) const;
    const UnitySceneObject *resolve(int file, const UValue *pointer) const;
};

struct UnitySceneSummary
{
    int meshes = 0;
    int skinned = 0;
    int bones = 0;
    int materials = 0;
    int textures = 0;
    int animations = 0;
    int skippedAnimations = 0;
};

namespace unityclass {
constexpr int GameObject = 1;
constexpr int Transform = 4;
constexpr int Material = 21;
constexpr int MeshRenderer = 23;
constexpr int Texture2D = 28;
constexpr int MeshFilter = 33;
constexpr int Mesh = 43;
constexpr int AnimationClip = 74;
constexpr int AnimatorController = 91;
constexpr int Animator = 95;
constexpr int SkinnedMeshRenderer = 137;
constexpr int AnimatorOverrideController = 221;
constexpr int RectTransform = 224;
}

std::vector<const UValue *> unityScenePointers(int classId, const UValue &value);
QStringList unityNameWords(const QString &name);
bool unityNameContains(const QString &name, const QString &part);
bool unityMaterialHasTextures(const UValue &material);
bool unityColorTextureName(const QString &name);
std::shared_ptr<Model3D> unitySceneModel(const UnityScene &scene, int file, qint64 rootPathId,
                                         UnitySceneSummary *summary, QString *error);
