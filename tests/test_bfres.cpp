#include "models/animationsearch.h"
#include "models/bfresreader.h"
#include "models/gltfwriter.h"
#include "core/registry.h"
#include "sample_bfres.h"
#include "textures/texturesearch.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <numbers>

namespace {

std::shared_ptr<const Model3D> modelOf(const Document &doc)
{
    for (const auto &t : doc.tables)
        if (t->hasModel())
            return t->model();
    return nullptr;
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

QJsonObject glbJson(const QByteArray &glb)
{
    const quint32 length = qFromLittleEndian<quint32>(glb.constData() + 12);
    return QJsonDocument::fromJson(glb.mid(20, length)).object();
}

bool near(float a, float b)
{
    return std::fabs(a - b) < 1e-4f;
}

}

class TestBfres : public QObject
{
    Q_OBJECT

private slots:
    void readsTheModelAndItsTables()
    {
        QString error;
        const auto doc = BfresReader().readBytes(samplebfres::make(), "sample.bfres", &error);
        QVERIFY2(doc, qPrintable(error));
        QStringList names;
        for (const auto &t : doc->tables)
            names << t->name();
        QCOMPARE(names, (QStringList{"Body", "Skeletal animations", "Materials", "tex_Alb"}));
        QVERIFY(doc->tables.front()->hasImage());
    }

    void placesRigidVerticesWithTheirBone()
    {
        const auto doc = BfresReader().readBytes(samplebfres::make(), "sample.bfres", nullptr);
        QVERIFY(doc);
        const auto model = modelOf(*doc);
        QVERIFY(model);
        QCOMPARE(model->nodes.size(), size_t(2));
        QCOMPARE(model->nodes[1].parent, 0);
        QVERIFY(near(model->nodes[1].rotation[2], float(std::sin(std::numbers::pi / 4))));
        QCOMPARE(model->primitives.size(), size_t(1));
        const ModelPrimitive &p = model->primitives[0];
        QCOMPARE(p.indices, (std::vector<quint32>{0, 1, 2}));
        QVERIFY(near(p.positions[3], 0) && near(p.positions[4], 2) && near(p.positions[5], 0));
        QVERIFY(near(p.positions[6], -1) && near(p.positions[7], 1));
        QCOMPARE(model->meshes[0].joints, std::vector<int>{1});
        QCOMPARE(p.weights[0], 1.f);
        QVERIFY(near(p.uv0[1], 1) && near(p.uv0[5], 0));
    }

    void picksTexturesBySamplerName()
    {
        const auto doc = BfresReader().readBytes(samplebfres::make(), "sample.bfres", nullptr);
        const auto model = modelOf(*doc);
        QVERIFY(model);
        const ModelMaterial &m = model->materials.at(0);
        QCOMPARE(model->textures.at(size_t(m.baseColorTexture)).name, QString("tex_Alb"));
        QCOMPARE(model->textures.at(size_t(m.normalTexture)).name, QString("tex_Nrm"));
        QCOMPARE(model->textures.at(size_t(m.normalTexture)).wrapU, 2);
        QCOMPARE(model->textures.at(size_t(m.normalTexture)).wrapV, 1);
    }

    void neverUsesADataTextureAsColour()
    {
        const auto mix = modelOf(*BfresReader().readBytes(samplebfres::make(false, false, "glass_Mix"), "a.bfres", nullptr));
        QVERIFY(mix);
        QCOMPARE(mix->materials.at(0).baseColorTexture, -1);
        const auto plain = modelOf(*BfresReader().readBytes(samplebfres::make(false, false, "wood"), "b.bfres", nullptr));
        QVERIFY(plain);
        QCOMPARE(plain->textures.at(size_t(plain->materials.at(0).baseColorTexture)).name, QString("wood"));
    }

    void usesEmbeddedTextures()
    {
        const auto doc = BfresReader().readBytes(samplebfres::make(), "sample.bfres", nullptr);
        const auto model = modelOf(*doc);
        QVERIFY(model && model->embeddedTexture);
        FormatRegistry registry;
        QStringList missing;
        const auto images = loadModelTextures(*model, registry, &missing);
        const int albedo = model->materials.at(0).baseColorTexture;
        QVERIFY(images.at(size_t(albedo)));
        QCOMPARE(images[size_t(albedo)]->width, 8);
        QCOMPARE(uchar(images[size_t(albedo)]->rgba[0]), uchar(200));
        QCOMPARE(missing, QStringList{"tex_Nrm"});
    }

    void listsSkeletalClips()
    {
        const std::vector<BfresClip> clips = BfresReader::skeletalClips(samplebfres::make());
        QCOMPARE(clips.size(), size_t(1));
        QCOMPARE(clips[0].name, QString("Wave"));
        QCOMPARE(clips[0].frames, 10);
        QVERIFY(clips[0].loop);
        QCOMPARE(clips[0].bones, (QStringList{"Arm", "Tail"}));
        QCOMPARE(clips[0].animatedBones, QStringList{"Arm"});
    }

    void samplesAnimationCurves()
    {
        const QByteArray data = samplebfres::make();
        const auto model = modelOf(*BfresReader().readBytes(data, "sample.bfres", nullptr));
        QString error;
        const auto animation = BfresReader::readSkeletalClip(data, "Wave", *model, &error);
        QVERIFY2(animation, qPrintable(error));
        const AnimationChannel *rotation = nullptr;
        for (const AnimationChannel &c : animation->channels) {
            QCOMPARE(c.node, 1);
            if (c.path == AnimationPath::Rotation)
                rotation = &c;
        }
        QVERIFY(rotation);
        QCOMPARE(rotation->times.size(), size_t(11));
        QVERIFY(near(rotation->times[5], 5.f / 30.f));
        QVERIFY(near(rotation->values[5 * 4 + 2], float(std::sin(std::numbers::pi / 8))));
        QVERIFY(near(rotation->values[10 * 4 + 2], float(std::sin(std::numbers::pi / 4))));
        QVERIFY(!BfresReader::readSkeletalClip(data, "Missing", *model, &error));
    }

    void readsVersion10Files()
    {
        const QByteArray data = samplebfres::makeV10();
        QString error;
        const auto doc = BfresReader().readBytes(data, "sample10.bfres", &error);
        QVERIFY2(doc, qPrintable(error));
        const auto model = modelOf(*doc);
        QVERIFY(model);
        QCOMPARE(model->nodes.size(), size_t(2));
        QCOMPARE(model->nodes[1].name, QString("Arm"));
        QCOMPARE(model->nodes[1].parent, 0);
        QVERIFY(near(model->nodes[1].translation[1], 1));
        QCOMPARE(model->primitives.size(), size_t(1));
        const ModelPrimitive &p = model->primitives.front();
        QCOMPARE(p.vertexCount(), qsizetype(3));
        QVERIFY(near(p.positions[3], 1));
        QVERIFY(near(p.uv0[0], 0.25f));
        QVERIFY(near(p.uv0[1], 0.5f));
        QCOMPARE(p.indices, (std::vector<quint32>{0, 1, 2}));
        QVERIFY(p.material >= 0);
        const ModelMaterial &m = model->materials[size_t(p.material)];
        QCOMPARE(m.name, QString("Skin"));
        QVERIFY(m.baseColorTexture >= 0);
        QCOMPARE(model->textures[size_t(m.baseColorTexture)].name, QString("tex_Alb"));

        const auto animation = BfresReader::readSkeletalClip(data, "Swing", *model, &error);
        QVERIFY2(animation, qPrintable(error));
        const AnimationChannel *rotation = nullptr;
        for (const AnimationChannel &c : animation->channels)
            if (c.path == AnimationPath::Rotation)
                rotation = &c;
        QVERIFY(rotation);
        QCOMPARE(rotation->times.size(), size_t(1101));
        auto zAt = [&](int frame) { return rotation->values[size_t(frame) * 4 + 2]; };
        QVERIFY(near(zAt(0), 0));
        QVERIFY(near(zAt(550), float(std::sin(std::numbers::pi / 8))));
        QVERIFY(near(zAt(275), float(std::sin(std::numbers::pi / 2 * 0.15625 / 2))));
        QVERIFY(near(zAt(1100), float(std::sin(std::numbers::pi / 4))));
    }

    void listsOtherAnimationsAndEmbeddedFiles()
    {
        const QByteArray data = samplebfres::makeV10Extras();
        QString error;
        const auto doc = BfresReader().readBytes(data, "camera.bfres", &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(doc->tables.size(), size_t(2));
        const Table &anims = *doc->tables[0];
        QCOMPARE(anims.rowCount(), 3);
        QCOMPARE(anims.text(0, 0, DisplayMode::Auto), QString("Blink"));
        QCOMPARE(anims.text(0, 2, DisplayMode::Auto), QString("750"));
        QCOMPARE(anims.text(0, 3, DisplayMode::Auto), QString("no"));
        QCOMPARE(anims.text(1, 0, DisplayMode::Auto), QString("Glow"));
        QCOMPARE(anims.text(1, 3, DisplayMode::Auto), QString("yes"));
        QCOMPARE(anims.text(2, 0, DisplayMode::Auto), QString("Hide"));
        const auto archive = doc->tables[1]->archive();
        QVERIFY(archive);
        QCOMPARE(archive->entries.size(), size_t(2));
        QCOMPARE(archive->entries[0].name, QString("Default.recb"));
        QCOMPARE(archive->entryData(0), QByteArray("camera preset"));
        QCOMPARE(archive->entries[1].name, QString("View01.bntx"));
        QCOMPARE(archive->entryData(1), QByteArray("BNTX\0\0\0\0", 8));
    }

    void registryPrefersBfresOverEmbeddedTextures()
    {
        FormatRegistry registry;
        const FormatReader *used = nullptr;
        QString error;
        const auto doc = registry.openBytes(samplebfres::make(), "output.bfres", &used, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(used->name(), BfresReader().name());
    }

    void rejectsDamagedAndUnsupportedFiles()
    {
        const QByteArray data = samplebfres::make();
        for (qsizetype size = 0x10; size < data.size(); size += 37) {
            QString error;
            const auto doc = BfresReader().readBytes(data.left(size), "cut.bfres", &error);
            if (doc)
                for (const auto &t : doc->tables)
                    if (t->hasModel())
                        t->model();
        }
        QByteArray bigEndian = data;
        bigEndian[0x0C] = char(0xFE);
        bigEndian[0x0D] = char(0xFF);
        QString error;
        QVERIFY(!BfresReader().readBytes(bigEndian, "wiiu.bfres", &error));
        QVERIFY(error.contains("Wii U"));
        QByteArray newer = data;
        newer[0x0A] = 11;
        QVERIFY(!BfresReader().readBytes(newer, "new.bfres", &error));
        QVERIFY(error.contains("11.0.0"));
    }

    void findsClipsInOtherFilesAndExportsThem()
    {
        QTemporaryDir dir;
        const QString modelPath = dir.filePath("Body.bfres");
        QVERIFY(writeFile(modelPath, samplebfres::make(false, true)));
        QVERIFY(writeFile(dir.filePath("BodyAnim.bfres"), samplebfres::make(true, false)));
        QVERIFY(writeFile(dir.filePath("Unrelated.bfres"), samplebfres::make(true, false)));
        const auto doc = BfresReader().read(modelPath, nullptr);
        QVERIFY(doc);
        const auto model = modelOf(*doc);
        QVERIFY(model);
        const std::vector<AnimationFile> found = findAnimationFiles(modelPath, *model);
        QCOMPARE(found.size(), size_t(1));
        QCOMPARE(found[0].clip, QString("Wave"));
        QCOMPARE(found[0].name, QString("Wave — BodyAnim"));
        QCOMPARE(found[0].matchedBones, 1);
        QCOMPARE(found[0].frames, 10);

        QStringList problems;
        const auto animations = loadAnimations(found, *model, &problems);
        QCOMPARE(animations.size(), size_t(1));
        QVERIFY(problems.isEmpty());
        const QJsonObject json = glbJson(writeGlb(*model, {}, {}, animations));
        QCOMPARE(json["animations"].toArray().size(), 1);
        QCOMPARE(json["skins"].toArray().size(), 1);
    }

    void writesDataColoursAsACustomAttribute()
    {
        Model3D model;
        model.meshes.push_back({});
        ModelPrimitive p;
        p.positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        p.indices = {0, 1, 2};
        p.colors = std::vector<quint8>(12, 255);
        p.colorsAreData = true;
        model.primitives.push_back(p);
        const QJsonObject attributes = glbJson(writeGlb(model, {}))["meshes"][0]["primitives"][0]["attributes"].toObject();
        QVERIFY(attributes.contains("_COLOR_0"));
        QVERIFY(!attributes.contains("COLOR_0"));
    }
};

QTEST_GUILESS_MAIN(TestBfres)
#include "test_bfres.moc"
