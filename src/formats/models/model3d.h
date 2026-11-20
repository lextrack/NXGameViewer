#pragma once

#include <QString>

#include <array>
#include <functional>
#include <memory>
#include <vector>

struct Image;

using Matrix4 = std::array<float, 16>;

struct ModelNode
{
    QString name;
    int parent = -1;
    std::array<float, 3> translation{0, 0, 0};
    std::array<float, 4> rotation{0, 0, 0, 1};
    std::array<float, 3> scale{1, 1, 1};
    std::array<float, 4> orientation{0, 0, 0, 1};
    int boneId = -1;
};

struct ModelTexture
{
    QString name;
    int wrapU = 0;
    int wrapV = 0;
};

enum class AlphaMode { Opaque, Mask, Blend };

struct ModelMaterial
{
    QString name;
    int baseColorTexture = -1;
    int normalTexture = -1;
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = true;
    bool auxiliary = false;
};

struct ModelPrimitive
{
    int mesh = 0;
    int material = -1;
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uv0;
    std::vector<float> uv1;
    std::vector<quint8> colors;
    std::vector<quint16> joints;
    std::vector<float> weights;
    std::vector<quint32> indices;
    bool environment = false;
    bool colorsAreData = false;

    qsizetype vertexCount() const { return qsizetype(positions.size() / 3); }
};

struct ModelMesh
{
    QString name;
    int node = -1;
    std::vector<int> joints;
    std::vector<Matrix4> inverseBinds;
    bool auxiliary = false;
};

enum class AnimationPath { Translation, Rotation, Scale };

struct AnimationChannel
{
    int node = -1;
    AnimationPath path = AnimationPath::Translation;
    std::vector<float> times;
    std::vector<float> values;
};

struct ModelAnimation
{
    QString name;
    std::vector<AnimationChannel> channels;
};

struct Model3D
{
    QString sourcePath;
    std::vector<ModelNode> nodes;
    std::vector<ModelMesh> meshes;
    std::vector<ModelPrimitive> primitives;
    std::vector<ModelMaterial> materials;
    std::vector<ModelTexture> textures;
    bool staticMeshesUseNodeTransform = false;
    std::function<std::shared_ptr<const Image>(const QString &name)> embeddedTexture;
    std::vector<ModelAnimation> animations;

    Matrix4 localMatrix(int node) const;
    Matrix4 worldMatrix(int node) const;
    Matrix4 meshMatrix(int mesh) const;
    bool isAuxiliary(const ModelPrimitive &primitive) const;
};

Matrix4 identityMatrix();
Matrix4 multiply(const Matrix4 &a, const Matrix4 &b);
Matrix4 inverted(const Matrix4 &m, bool *ok = nullptr);
std::array<float, 3> transformPoint(const Matrix4 &m, const float *p);
std::array<float, 4> quaternionFromEulerZYX(float x, float y, float z);
std::array<float, 4> multiplyQuaternions(const std::array<float, 4> &a, const std::array<float, 4> &b);
std::array<float, 4> normalizedQuaternion(const std::array<float, 4> &q);
