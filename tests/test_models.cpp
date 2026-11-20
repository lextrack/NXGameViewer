#include "models/animationsearch.h"
#include "models/falcommdlreader.h"
#include "models/gltfwriter.h"
#include "sample_mdl.h"
#include "textures/texturesearch.h"
#include "models/meshpreview.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <cstring>


namespace {

using sample::makeModel;
using sample::makeRichModel;


std::shared_ptr<const Model3D> richModel(const QString &path = {})
{
    QString error;
    const auto doc = FalcomMdlReader::fromBytes(makeRichModel(), &error, path);
    return doc ? doc->tables.front()->model() : nullptr;
}

QJsonObject glbJson(const QByteArray &glb, QByteArray *bin = nullptr)
{
    if (glb.size() < 20 || qFromLittleEndian<quint32>(glb.constData()) != 0x46546C67)
        return {};
    const quint32 jsonLength = qFromLittleEndian<quint32>(glb.constData() + 12);
    if (bin)
        *bin = glb.mid(20 + jsonLength + 8);
    return QJsonDocument::fromJson(glb.mid(20, jsonLength)).object();
}

template <typename T>
std::vector<T> accessorValues(const QJsonObject &json, const QByteArray &bin, int index)
{
    const QJsonObject accessor = json["accessors"].toArray()[index].toObject();
    const QJsonObject view = json["bufferViews"].toArray()[accessor["bufferView"].toInt()].toObject();
    const QString type = accessor["type"].toString();
    const int components = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : type == "VEC4" ? 4 : 16;
    std::vector<T> values(size_t(accessor["count"].toInt() * components));
    std::memcpy(values.data(), bin.constData() + view["byteOffset"].toInt(), values.size() * sizeof(T));
    return values;
}

int skinnedMeshIndex(const Model3D &model)
{
    for (const ModelPrimitive &prim : model.primitives)
        if (!prim.joints.empty() && !model.meshes[size_t(prim.mesh)].joints.empty())
            return prim.mesh;
    return -1;
}

void checkSkinsAreValid(const QJsonObject &json, const QByteArray &bin)
{
    const QJsonArray skins = json["skins"].toArray();
    QVERIFY(!skins.isEmpty());
    for (const QJsonValue &node : json["nodes"].toArray()) {
        if (!node.toObject().contains("skin"))
            continue;
        const QJsonArray joints = skins[node.toObject()["skin"].toInt()].toObject()["joints"].toArray();
        QSet<int> unique;
        for (const QJsonValue &j : joints)
            unique.insert(j.toInt());
        QCOMPARE(unique.size(), joints.size());

        const QJsonObject mesh = json["meshes"].toArray()[node.toObject()["mesh"].toInt()].toObject();
        for (const QJsonValue &p : mesh["primitives"].toArray()) {
            const QJsonObject attributes = p.toObject()["attributes"].toObject();
            QVERIFY(attributes.contains("JOINTS_0"));
            QVERIFY(attributes.contains("WEIGHTS_0"));
            const auto jointValues = accessorValues<quint16>(json, bin, attributes["JOINTS_0"].toInt());
            const auto weightValues = accessorValues<float>(json, bin, attributes["WEIGHTS_0"].toInt());
            QCOMPARE(jointValues.size(), weightValues.size());
            for (size_t v = 0; v < jointValues.size(); v += 4) {
                float sum = 0;
                for (size_t k = 0; k < 4; ++k) {
                    QVERIFY(jointValues[v + k] < joints.size());
                    sum += weightValues[v + k];
                    for (size_t e = v; e < v + k; ++e)
                        QVERIFY(!(weightValues[e] > 0 && weightValues[v + k] > 0 && jointValues[e] == jointValues[v + k]));
                }
                QVERIFY(std::abs(sum - 1.f) < 1e-4f);
            }
        }
    }
}

QByteArray sampleAnimation(bool withRoot = true)
{
    const float h = std::sqrt(0.5f);
    std::vector<sample::SampleCurve> curves;
    if (withRoot) {
        curves.push_back({"Root", 9, {{10.f, 0, 1, 0}, {11.f, 0, 2, 0}}});
        curves.push_back({"Root", 10, {{10.f, 0, 0, 0, 1}, {11.f, 0, h, 0, h}}});
        curves.push_back({"Root", 16, {{10.f, 1.f}}});
    }
    curves.push_back({"Ghost", 10, {{10.f, 0, 0, 0, 1}}});
    return sample::makeAnimation(curves, 10.f, 11.f);
}

int nodeNamed(const Model3D &model, const QString &name)
{
    for (size_t i = 0; i < model.nodes.size(); ++i)
        if (model.nodes[i].name == name)
            return int(i);
    return -1;
}

bool nearlyEqual(const float *a, const std::array<float, 4> &b)
{
    for (size_t k = 0; k < 4; ++k)
        if (std::abs(a[k] - b[k]) > 1e-5f)
            return false;
    return true;
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

const Table *tableNamed(const Document &doc, const QString &name)
{
    for (const auto &t : doc.tables)
        if (t->name() == name)
            return t.get();
    return nullptr;
}

bool isBackground(const Image &image, int x, int y)
{
    const auto *p = reinterpret_cast<const uchar *>(image.rgba.constData()) + (y * image.width + x) * 4;
    return p[0] == 48 && p[1] == 52 && p[2] == 58;
}

}

class TestModels : public QObject
{
    Q_OBJECT

private slots:
    void vertexColoursAreKeptAsData()
    {
        QString error;
        const auto doc = FalcomMdlReader::fromBytes(makeRichModel(), &error);
        QVERIFY2(doc, qPrintable(error));
        const auto model = doc->tables.front()->model();
        QVERIFY(model);
        bool coloured = false;
        for (const ModelPrimitive &p : model->primitives)
            if (!p.colors.empty()) {
                coloured = true;
                QVERIFY(p.colorsAreData);
            }
        QVERIFY(coloured);
        bool custom = false;
        for (const QJsonValue &mesh : glbJson(writeGlb(*model, {}))["meshes"].toArray())
            for (const QJsonValue &prim : mesh["primitives"].toArray()) {
                QVERIFY(!prim["attributes"].toObject().contains("COLOR_0"));
                custom = custom || prim["attributes"].toObject().contains("_COLOR_0");
            }
        QVERIFY(custom);
    }

    void readsModel()
    {
        QString error;
        const auto doc = FalcomMdlReader::fromBytes(makeModel(), &error);
        QVERIFY2(doc, qPrintable(error));
        const Table *model = tableNamed(*doc, "Model");
        QVERIFY(model);
        QCOMPARE(model->rowCount(), 7);  // no "problems" row
        QCOMPARE(model->description(), QString("3 vertices, 1 triangles"));
    }

    void readsNodes()
    {
        QString error;
        const auto doc = FalcomMdlReader::fromBytes(makeModel(), &error);
        QVERIFY2(doc, qPrintable(error));
        const Table *nodes = tableNamed(*doc, "Nodes");
        QVERIFY(nodes);
        QCOMPARE(nodes->rowCount(), 2);
        // name, kind, mesh, parent, position, rotation, scale, children
        QCOMPARE(nodes->text(0, 2, DisplayMode::Auto), QString());  // -1: no mesh
        QCOMPARE(nodes->text(1, 1, DisplayMode::Auto), QString("mesh"));
        QCOMPARE(nodes->text(1, 3, DisplayMode::Auto), QString("world"));
        QCOMPARE(nodes->text(1, 6, DisplayMode::Auto), QString("1, 1, 1"));
        QCOMPARE(nodes->text(0, 7, DisplayMode::Auto), QString("1"));
    }

    void readsMaterialsAndMeshes()
    {
        QString error;
        const auto doc = FalcomMdlReader::fromBytes(makeModel(), &error);
        QVERIFY2(doc, qPrintable(error));
        const Table *materials = tableNamed(*doc, "Materials");
        QVERIFY(materials);
        QCOMPARE(materials->text(0, 2, DisplayMode::Auto), QString("tex_a"));
        QCOMPARE(materials->text(0, 3, DisplayMode::Auto), QString("2"));
        const Table *parameters = tableNamed(*doc, "Material parameters");
        QVERIFY(parameters);
        QCOMPARE(parameters->text(0, 2, DisplayMode::Auto), QString("1, 0.5, 0"));
        QCOMPARE(parameters->text(1, 2, DisplayMode::Auto), QString("1"));
        const Table *meshes = tableNamed(*doc, "Meshes");
        QVERIFY(meshes);
        // mesh, material, vertices, triangles, data
        QCOMPARE(meshes->text(0, 0, DisplayMode::Auto), QString("body"));
        QCOMPARE(meshes->text(0, 2, DisplayMode::Auto), QString("mat"));
        QCOMPARE(meshes->text(0, 3, DisplayMode::Auto), QString("3"));
        QCOMPARE(meshes->text(0, 6, DisplayMode::Auto), QString("positions, indices"));
    }

    void rendersPreview()
    {
        QString error;
        const auto doc = FalcomMdlReader::fromBytes(makeModel(), &error);
        QVERIFY2(doc, qPrintable(error));
        const auto image = tableNamed(*doc, "Model")->image();
        QVERIFY(image);
        QCOMPARE(image->width, 640);
        QVERIFY(!isBackground(*image, 320, 330));  // the triangle fills the middle
        QVERIFY(isBackground(*image, 5, 5));       // corners stay empty
    }

    void previewSkipsBadIndices()
    {
        PreviewMesh mesh;
        mesh.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        mesh.indices = {0, 1, 7};  // 7 does not exist
        const Image image = renderPreview({mesh}, 64);
        QCOMPARE(image.rgba.size(), qsizetype(64 * 64 * 4));
        QVERIFY(isBackground(image, 32, 32));
    }

    void previewStopsAtItsBudget()
    {
        // 1,000 triangles that each cover the whole picture: about 4 million pixel tests.
        PreviewMesh mesh;
        mesh.positions = {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}};
        for (int i = 0; i < 1000; ++i)
            mesh.indices.insert(mesh.indices.end(), {0, 1, 2});
        bool complete = false;
        renderPreview({mesh}, 64, kDefaultPreviewWork, &complete);
        QVERIFY(complete);
        const Image partial = renderPreview({mesh}, 64, 10'000, &complete);
        QVERIFY(!complete);
        QCOMPARE(partial.rgba.size(), qsizetype(64 * 64 * 4));  // still a whole picture
    }

    void everyMeshKeepsItsNameAndMaterial()
    {
        QString error;
        const auto doc = FalcomMdlReader::fromBytes(makeRichModel(), &error);
        QVERIFY2(doc, qPrintable(error));
        const Table *meshes = tableNamed(*doc, "Meshes");
        QVERIFY(meshes);
        QCOMPARE(meshes->rowCount(), 3);
        QCOMPARE(meshes->text(1, 0, DisplayMode::Auto), QString("prop"));
        QCOMPARE(meshes->text(1, 2, DisplayMode::Auto), QString("hair"));
        QCOMPARE(meshes->text(2, 0, DisplayMode::Auto), QString("body_shadow"));
        QCOMPARE(meshes->text(0, 5, DisplayMode::Auto), QString("1"));
    }

    void buildsAModel()
    {
        const auto model = richModel();
        QVERIFY(model);
        QCOMPARE(model->meshes.size(), size_t(3));
        QCOMPARE(model->primitives.size(), size_t(3));
        QVERIFY(model->meshes[2].auxiliary);
        QCOMPARE(model->meshes[0].joints, std::vector<int>{1});
        QCOMPARE(model->meshes[1].node, 2);

        const ModelPrimitive &body = model->primitives[0];
        QCOMPARE(body.vertexCount(), qsizetype(3));
        QCOMPARE(body.normals[1], 1.f);
        QCOMPARE(body.uv0[4], 0.5f);
        QCOMPARE(body.weights[0], 0.5f);
        QCOMPARE(body.weights[1], 0.5f);

        QCOMPARE(model->materials[0].alphaMode, AlphaMode::Opaque);
        QCOMPARE(model->materials[1].alphaMode, AlphaMode::Mask);
        QCOMPARE(model->materials[1].alphaCutoff, 0.3f);
        QVERIFY(!model->materials[1].doubleSided);
        QCOMPARE(model->textures[size_t(model->materials[1].baseColorTexture)].name, QString("tex_hair"));
        QCOMPARE(model->textures[1].wrapU, 2);
    }

    void staticMeshesFollowTheirNode()
    {
        const auto model = richModel();
        QVERIFY(model);
        const Matrix4 prop = model->meshMatrix(1);
        QCOMPARE(prop[12], 5.f);
        const Matrix4 skinned = model->meshMatrix(0);
        QCOMPARE(skinned[12], 0.f);
    }

    void nodeRotationsAreZyxEuler()
    {
        const Matrix4 root = richModel()->worldMatrix(1);
        const float up[3] = {0, 1, 0};
        const auto p = transformPoint(root, up);
        QVERIFY(std::abs(p[0]) < 1e-5f);
        QVERIFY(std::abs(p[1] - 1.f) < 1e-5f);
        QVERIFY(std::abs(p[2] - 1.f) < 1e-5f);

        const auto q = quaternionFromEulerZYX(0.3f, -0.7f, 1.1f);
        const auto rz = quaternionFromEulerZYX(0, 0, 1.1f), ry = quaternionFromEulerZYX(0, -0.7f, 0),
                   rx = quaternionFromEulerZYX(0.3f, 0, 0);
        auto mul = [](const std::array<float, 4> &a, const std::array<float, 4> &b) {
            return std::array<float, 4>{a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                                        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                                        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                                        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
        };
        const auto expected = mul(mul(rz, ry), rx);
        for (int i = 0; i < 4; ++i)
            QVERIFY(std::abs(q[size_t(i)] - expected[size_t(i)]) < 1e-5f);
    }

    void environmentBoxesAreHidden()
    {
        QString error;
        const auto doc = FalcomMdlReader::fromBytes(makeRichModel(true), &error);
        QVERIFY2(doc, qPrintable(error));
        const auto model = doc->tables.front()->model();
        QVERIFY(model);
        QCOMPARE(model->primitives.size(), size_t(4));
        QVERIFY(model->isAuxiliary(model->primitives[3]));
        QVERIFY(!model->isAuxiliary(model->primitives[1]));
        QCOMPARE(glbJson(writeGlb(*model, {}))["meshes"].toArray().size(), 2);
    }

    void exportsGlb()
    {
        const auto model = richModel();
        QVERIFY(model);
        auto texture = std::make_shared<Image>();
        texture->width = 2;
        texture->height = 1;
        texture->rgba = QByteArray("\xff\x00\x00\xff\x00\x00\xff\x40", 8);
        std::vector<std::shared_ptr<const Image>> textures(model->textures.size());
        textures[1] = texture;

        QByteArray bin;
        GltfOptions keepWrapModes;
        keepWrapModes.bakeMirroredRepeat = false;
        const QJsonObject json = glbJson(writeGlb(*model, textures, keepWrapModes), &bin);
        QCOMPARE(json["asset"].toObject()["version"].toString(), QString("2.0"));
        QCOMPARE(json["meshes"].toArray().size(), 2);
        QCOMPARE(json["skins"].toArray().size(), 1);
        QCOMPARE(json["buffers"].toArray()[0].toObject()["byteLength"].toInteger(), bin.size());

        const QJsonArray materials = json["materials"].toArray();
        QCOMPARE(materials[1].toObject()["alphaMode"].toString(), QString("MASK"));
        QCOMPARE(materials[1].toObject()["doubleSided"].toBool(), false);
        QVERIFY(!materials[0].toObject().contains("alphaMode"));
        QCOMPARE(json["samplers"].toArray()[0].toObject()["wrapS"].toInt(), 33071);

        const QJsonObject image = json["images"].toArray()[0].toObject();
        const QJsonObject view = json["bufferViews"].toArray()[image["bufferView"].toInt()].toObject();
        const QImage png = QImage::fromData(bin.mid(view["byteOffset"].toInt(), view["byteLength"].toInt()), "PNG");
        QCOMPARE(png.size(), QSize(2, 1));
        QCOMPARE(png.pixelColor(1, 0), QColor(0, 0, 255, 0x40));

        const QJsonArray nodes = json["nodes"].toArray();
        QCOMPARE(nodes[2].toObject()["children"].toArray().size(), 1);
        const int joint = json["skins"].toArray()[0].toObject()["joints"].toArray()[0].toInt();
        QCOMPARE(nodes[joint].toObject()["name"].toString(), QString("Root"));
    }

    void bakesMirroredRepeatIntoTheTexture()
    {
        const auto model = richModel();
        QVERIFY(model);
        const ModelTexture &info = model->textures[1];
        QCOMPARE(info.wrapU, 2);
        QCOMPARE(info.wrapV, 1);
        auto texture = std::make_shared<Image>();
        texture->width = 2;
        texture->height = 1;
        texture->rgba = QByteArray("\xff\x00\x00\xff\x00\x00\xff\x40", 8);
        std::vector<std::shared_ptr<const Image>> textures(model->textures.size());
        textures[1] = texture;

        QByteArray bin;
        const QJsonObject json = glbJson(writeGlb(*model, textures), &bin);
        const QJsonObject sampler = json["samplers"].toArray()[0].toObject();
        QCOMPARE(sampler["wrapS"].toInt(), 33071);
        QCOMPARE(sampler["wrapT"].toInt(), 10497);
        const QJsonObject image = json["images"].toArray()[0].toObject();
        const QJsonObject view = json["bufferViews"].toArray()[image["bufferView"].toInt()].toObject();
        const QImage png = QImage::fromData(bin.mid(view["byteOffset"].toInt(), view["byteLength"].toInt()), "PNG");
        QCOMPARE(png.size(), QSize(2, 2));
        QCOMPARE(png.pixelColor(1, 1), QColor(0, 0, 255, 0x40));

        int material = -1;
        for (size_t i = 0; i < model->materials.size(); ++i)
            if (model->materials[i].baseColorTexture == 1)
                material = int(i);
        QVERIFY(material >= 0);
        const ModelPrimitive *prim = nullptr;
        for (const ModelPrimitive &p : model->primitives)
            if (p.material == material && !p.uv0.empty())
                prim = &p;
        QVERIFY(prim);
        const QJsonArray meshes = json["meshes"].toArray();
        bool checked = false;
        for (const QJsonValue &mesh : meshes)
            for (const QJsonValue &p : mesh["primitives"].toArray()) {
                if (p["material"].toInt() != material)
                    continue;
                const QJsonObject accessor = json["accessors"].toArray()[p["attributes"]["TEXCOORD_0"].toInt()].toObject();
                const QJsonObject uvView = json["bufferViews"].toArray()[accessor["bufferView"].toInt()].toObject();
                float u, v;
                std::memcpy(&u, bin.constData() + uvView["byteOffset"].toInt(), 4);
                std::memcpy(&v, bin.constData() + uvView["byteOffset"].toInt() + 4, 4);
                QCOMPARE(u, prim->uv0[0]);
                QCOMPARE(v, (1.f - prim->uv0[1]) * 0.5f);
                checked = true;
            }
        QVERIFY(checked);
    }

    void exportedSkinKeepsTheBindPose()
    {
        const auto model = richModel();
        QByteArray bin;
        const QJsonObject json = glbJson(writeGlb(*model, {}), &bin);
        const QJsonObject skin = json["skins"].toArray()[0].toObject();
        const QJsonObject accessor = json["accessors"].toArray()[skin["inverseBindMatrices"].toInt()].toObject();
        const QJsonObject view = json["bufferViews"].toArray()[accessor["bufferView"].toInt()].toObject();
        Matrix4 ibm;
        std::memcpy(ibm.data(), bin.constData() + view["byteOffset"].toInt(), 64);
        const Matrix4 product = multiply(model->worldMatrix(1), ibm);
        const Matrix4 identity = identityMatrix();
        for (int i = 0; i < 16; ++i)
            QVERIFY(std::abs(product[size_t(i)] - identity[size_t(i)]) < 1e-5f);
    }

    void exportFlipsTextureV()
    {
        const auto model = richModel();
        QByteArray bin;
        const QJsonObject json = glbJson(writeGlb(*model, {}), &bin);
        const QJsonObject primitive = json["meshes"].toArray()[0].toObject()["primitives"].toArray()[0].toObject();
        const QJsonObject accessor = json["accessors"].toArray()[primitive["attributes"].toObject()["TEXCOORD_0"].toInt()].toObject();
        const QJsonObject view = json["bufferViews"].toArray()[accessor["bufferView"].toInt()].toObject();
        float uv[6];
        std::memcpy(uv, bin.constData() + view["byteOffset"].toInt(), sizeof uv);
        QCOMPARE(uv[1], 1.f);
        QCOMPARE(uv[5], 0.f);
        QVERIFY(primitive["attributes"].toObject().contains("JOINTS_0"));
    }

    void exportMergesRepeatedJoints()
    {
        const auto source = richModel();
        QVERIFY(source);
        Model3D model = *source;
        const int mesh = skinnedMeshIndex(model);
        QVERIFY(mesh >= 0);
        std::vector<int> &joints = model.meshes[size_t(mesh)].joints;
        const quint16 repeated = quint16(joints.size());
        joints.push_back(joints[0]);
        joints.push_back(int(model.nodes.size()) + 5);
        for (ModelPrimitive &prim : model.primitives) {
            if (prim.mesh != mesh || prim.joints.empty())
                continue;
            prim.joints[0] = 0;
            prim.joints[1] = repeated;
            prim.weights[0] = 0.25f;
            prim.weights[1] = 0.75f;
            prim.weights[2] = 0.f;
            prim.weights[3] = 0.f;
        }

        QByteArray bin;
        const QJsonObject json = glbJson(writeGlb(model, {}), &bin);
        checkSkinsAreValid(json, bin);
    }

    void exportSkinsEveryPrimitiveOfASkinnedMesh()
    {
        const auto source = richModel();
        QVERIFY(source);
        Model3D model = *source;
        const int mesh = skinnedMeshIndex(model);
        QVERIFY(mesh >= 0);
        ModelPrimitive loose;
        for (const ModelPrimitive &prim : model.primitives)
            if (prim.mesh == mesh && !prim.joints.empty()) {
                loose = prim;
                break;
            }
        loose.joints.clear();
        loose.weights.clear();
        model.primitives.push_back(loose);

        QByteArray bin;
        const QJsonObject json = glbJson(writeGlb(model, {}), &bin);
        checkSkinsAreValid(json, bin);
    }

    void textureSearchSeesNewFolders()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("asset/common/model"));
        const QString modelPath = dir.filePath("asset/common/model/m.mdl");
        QVERIFY(findTextureFile(modelPath, "tex_new").isEmpty());

        QVERIFY(QDir(dir.path()).mkpath("asset/nx/image"));
        QFile texture(dir.filePath("asset/nx/image/tex_new.png"));
        QVERIFY(texture.open(QIODevice::WriteOnly));
        texture.close();
        QCOMPARE(findTextureFile(modelPath, "tex_new"), texture.fileName());
    }

    void textureSearchPrefersRealTextureFolders()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("asset/common/model"));
        QVERIFY(QDir(dir.path()).mkpath("asset/atext"));
        QVERIFY(QDir(dir.path()).mkpath("asset/image"));
        for (const char *folder : {"asset/atext", "asset/image"}) {
            QFile f(dir.filePath(QString(folder) + "/tex_body.png"));
            QVERIFY(f.open(QIODevice::WriteOnly));
        }
        QCOMPARE(findTextureFile(dir.filePath("asset/common/model/m.mdl"), "tex_body"),
                 dir.filePath("asset/image/tex_body.png"));
    }

    void restPoseIncludesTheRotateQuaternion()
    {
        const float h = std::sqrt(0.5f);
        QString error;
        const auto doc = FalcomMdlReader::fromBytes(makeRichModel(false, {0, h, 0, h}), &error);
        QVERIFY2(doc, qPrintable(error));
        const auto model = doc->tables.front()->model();
        QVERIFY(model);
        const int root = nodeNamed(*model, "Root");
        QVERIFY(root >= 0);
        const auto orientation = quaternionFromEulerZYX(1.5707964f, 0, 0);
        QVERIFY(nearlyEqual(model->nodes[size_t(root)].orientation.data(), orientation));
        QVERIFY(nearlyEqual(model->nodes[size_t(root)].rotation.data(), multiplyQuaternions(orientation, {0, h, 0, h})));
    }

    void readsAnimationCurves()
    {
        const auto model = richModel();
        QVERIFY(model);
        QString error;
        const auto animation = FalcomMdlReader::readAnimation(sampleAnimation(), *model, "run", &error);
        QVERIFY2(animation, qPrintable(error));
        QCOMPARE(animation->name, QString("run"));
        QCOMPARE(animation->channels.size(), size_t(2));
        const int root = nodeNamed(*model, "Root");
        const AnimationChannel &move = animation->channels[0];
        QCOMPARE(move.node, root);
        QCOMPARE(move.path, AnimationPath::Translation);
        QCOMPARE(move.times, (std::vector<float>{0.f, 1.f}));
        QCOMPARE(move.values, (std::vector<float>{0, 1, 0, 0, 2, 0}));

        const float h = std::sqrt(0.5f);
        const AnimationChannel &turn = animation->channels[1];
        QCOMPARE(turn.path, AnimationPath::Rotation);
        QCOMPARE(turn.values.size(), size_t(8));
        const auto orientation = model->nodes[size_t(root)].orientation;
        QVERIFY(nearlyEqual(turn.values.data(), orientation));
        QVERIFY(nearlyEqual(turn.values.data() + 4, multiplyQuaternions(orientation, {0, h, 0, h})));
    }

    void listsAnimatedBonesOfAnimationFilesOnly()
    {
        QCOMPARE(FalcomMdlReader::animatedNodeNames(sampleAnimation()), QStringList({"Root", "Ghost"}));
        QVERIFY(FalcomMdlReader::animatedNodeNames(makeRichModel()).isEmpty());
        QVERIFY(FalcomMdlReader::animatedNodeNames("junk").isEmpty());
    }

    void animationWithoutMatchingBonesIsRejected()
    {
        const auto model = richModel();
        QString error;
        QVERIFY(!FalcomMdlReader::readAnimation(sampleAnimation(false), *model, "x", &error));
        QVERIFY(!error.isEmpty());
    }

    void exportsAnimations()
    {
        const auto model = richModel();
        const auto animation = FalcomMdlReader::readAnimation(sampleAnimation(), *model, "run", nullptr);
        QVERIFY(animation);
        QByteArray bin;
        const QJsonObject json = glbJson(writeGlb(*model, {}, {}, {*animation}), &bin);
        const QJsonArray animations = json["animations"].toArray();
        QCOMPARE(animations.size(), 1);
        const QJsonObject a = animations[0].toObject();
        QCOMPARE(a["name"].toString(), QString("run"));
        const QJsonArray channels = a["channels"].toArray();
        QCOMPARE(channels.size(), 2);
        const QJsonArray nodes = json["nodes"].toArray();
        for (const QJsonValue &c : channels) {
            const QJsonObject target = c.toObject()["target"].toObject();
            QCOMPARE(nodes[target["node"].toInt()].toObject()["name"].toString(), QString("Root"));
        }
        QCOMPARE(channels[1].toObject()["target"].toObject()["path"].toString(), QString("rotation"));
        const QJsonObject sampler = a["samplers"].toArray()[1].toObject();
        QCOMPARE(sampler["interpolation"].toString(), QString("LINEAR"));
        const QJsonObject input = json["accessors"].toArray()[sampler["input"].toInt()].toObject();
        QCOMPARE(input["min"].toArray()[0].toDouble(), 0.0);
        QCOMPARE(input["max"].toArray()[0].toDouble(), 1.0);
        QCOMPARE(accessorValues<float>(json, bin, sampler["output"].toInt()), animation->channels[1].values);
    }

    void truncatedAnimationsNeverCrash()
    {
        const auto model = richModel();
        const QByteArray full = sampleAnimation();
        for (qsizetype n = 0; n < full.size(); ++n) {
            FalcomMdlReader::animatedNodeNames(full.left(n));
            if (const auto animation = FalcomMdlReader::readAnimation(full.left(n), *model, "x", nullptr))
                writeGlb(*model, {}, {}, {*animation});
        }
    }

    void findsAnimationFilesNextToTheModel()
    {
        QTemporaryDir dir;
        const QString modelPath = dir.filePath("chr9_c00.mdl");
        QVERIFY(writeFile(modelPath, makeRichModel()));
        QVERIFY(writeFile(dir.filePath("chr9_c01.mdl"), makeRichModel()));
        QVERIFY(writeFile(dir.filePath("chr9_m_run.mdl"), sampleAnimation()));
        QVERIFY(writeFile(dir.filePath("chr9_m_props.mdl"), sampleAnimation(false)));
        QVERIFY(writeFile(dir.filePath("other_m_run.mdl"), sampleAnimation()));
        QString error;
        const auto doc = FalcomMdlReader().read(modelPath, &error);
        QVERIFY2(doc, qPrintable(error));
        const auto model = doc->tables.front()->model();
        QVERIFY(model);

        const std::vector<AnimationFile> found = findAnimationFiles(modelPath, *model);
        QCOMPARE(found.size(), size_t(1));
        QCOMPARE(found[0].name, QString("chr9_m_run"));
        QCOMPARE(found[0].matchedBones, 1);
        QCOMPARE(found[0].bones, 2);

        QStringList problems;
        const auto animations =
            loadAnimations({{dir.filePath("chr9_m_run.mdl")}, {dir.filePath("chr9_m_props.mdl")}}, *model, &problems);
        QCOMPARE(animations.size(), size_t(1));
        QCOMPARE(problems.size(), 1);
    }

    void findsTexturesNearTheModel()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("asset/common/model"));
        QVERIFY(QDir(dir.path()).mkpath("asset/nx/image"));
        const QString modelPath = dir.filePath("asset/common/model/m.mdl");
        QFile texture(dir.filePath("asset/nx/image/tex_hair.png"));
        QVERIFY(texture.open(QIODevice::WriteOnly));
        QImage(4, 4, QImage::Format_RGBA8888).save(&texture, "PNG");
        texture.close();

        QCOMPARE(findTextureFile(modelPath, "tex_hair"), texture.fileName());
        QVERIFY(findTextureFile(modelPath, "tex_skin").isEmpty());
        QVERIFY(findTextureFile(modelPath, "../../secret").isEmpty());
    }

    void truncatedModelsNeverCrash()
    {
        const QByteArray full = makeRichModel();
        for (qsizetype n = 0; n < full.size(); ++n) {
            QString error;
            if (const auto doc = FalcomMdlReader::fromBytes(full.left(n), &error)) {
                for (const auto &t : doc->tables) {
                    t->image();  // rendering only uses data that is really there
                    if (const auto model = t->model())
                        writeGlb(*model, {});
                }
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestModels)
#include "test_models.moc"
