#include "data/asbreader.h"

#include "core/bytes.h"
#include "core/simpletable.h"
#include "core/storedreader.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QObject>

#include <array>
#include <vector>

namespace {

constexpr qsizetype kCommandSize = 0x30;
constexpr qsizetype kNodeSize = 0x24;
constexpr qsizetype kOldHeaderSize = 0x6C;
constexpr int kMaxItems = 1 << 20;

class Reader : public ByteReader
{
public:
    using ByteReader::ByteReader;
    int count(quint32 n) const { return ByteReader::count(n, kMaxItems); }
};

QString nodeTypeName(int type)
{
    static const std::array<const char *, 26> names{
        "", "FloatSelector", "StringSelector", "SkeletalAnimation", "State", "Unknown5", "OneDimensionalBlender",
        "Sequential", "IntSelector", "Simultaneous", "Event", "MaterialAnimation", "FrameController",
        "DummyAnimation", "RandomSelector", "Unknown15", "PreviousTagSelector", "BonePositionSelector",
        "BoneAnimation", "InitialFrame", "BoneBlender", "BoolSelector", "Alert", "SubtractAnimation",
        "ShapeAnimation", "Unknown25"};
    if (type > 0 && size_t(type) < names.size())
        return QString::fromLatin1(names[size_t(type)]);
    return QStringLiteral("type %1").arg(type);
}

bool namesAnimation(int type)
{
    return type == 3 || type == 18;
}

}

int AsbReader::score(const QString &, QByteArrayView head) const
{
    return head.startsWith("ASB ") ? 100 : 0;
}

std::unique_ptr<Document> AsbReader::read(const QString &path, QString *error) const
{
    const std::optional<QByteArray> bytes = readWholeFile(path, kMaxFileSize, error);
    if (!bytes)
        return nullptr;
    return readBytes(*bytes, path, error);
}

std::unique_ptr<Document> AsbReader::readBytes(const QByteArray &data, const QString &, QString *error) const
{
    std::vector<SimpleTable::Row> commandRows, nodeRows, animationRows;
    QJsonArray commandJson, nodeJson, animationJson;
    QString name;
    quint32 version = 0;
    try {
        Reader r(data);
        if (data.size() < kOldHeaderSize || !data.startsWith("ASB "))
            throw ParseFailure(QObject::tr("Not an animation sequence (no \"ASB \" at the start).").toStdString());
        version = r.u32(4);
        int commandCount = 0, nodeCount = 0;
        qint64 commands = 0, nodes = 0;
        if (version >= 0x41B) {
            commandCount = r.count(r.u32(0x0C));
            commands = r.u32(0x10);
            nodeCount = r.count(r.u32(0x14));
            nodes = r.u32(0x18);
            r.setPool(r.u32(0x3C));
        } else {
            commandCount = r.count(r.u32(0x0C));
            nodeCount = r.count(r.u32(0x10));
            commands = kOldHeaderSize;
            nodes = kOldHeaderSize + qint64(commandCount) * kCommandSize;
            r.setPool(r.u32(0x24));
        }
        name = r.str(r.u32(0x08));

        std::vector<int> types;
        std::vector<QString> animations;
        for (int i = 0; i < nodeCount; ++i) {
            const qint64 n = nodes + qint64(i) * kNodeSize;
            const int type = r.u16(n);
            QString animation;
            if (namesAnimation(type)) {
                const qint64 body = r.u32(n + 8);
                const qint32 flags = qint32(r.u32(body));
                animation = flags < 0 ? QObject::tr("(from blackboard #%1)").arg(quint32(flags) & 0xFFFF) : r.str(r.u32(body + 4));
            }
            types.push_back(type);
            animations.push_back(animation);
        }
        auto nodeRef = [&](int index) {
            if (index < 0 || size_t(index) >= types.size())
                return QStringLiteral("#%1").arg(index);
            const QString &animation = animations[size_t(index)];
            return QStringLiteral("#%1 %2").arg(index).arg(nodeTypeName(types[size_t(index)]))
                 + (animation.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(animation));
        };

        for (int i = 0; i < commandCount; ++i) {
            const qint64 c = commands + qint64(i) * kCommandSize;
            const QString command = r.str(r.u32(c));
            const int root = r.u16(c + 0x2C);
            const QString guid = r.guid(c + 0x1C);
            commandRows.push_back({command, nodeRef(root), guid});
            commandJson.append(QJsonObject{{"name", command}, {"node", root}, {"guid", guid}});
        }

        QMap<QString, int> uses;
        for (int i = 0; i < nodeCount; ++i) {
            const qint64 n = nodes + qint64(i) * kNodeSize;
            const QString guid = r.guid(n + 0x14);
            const QString &animation = animations[size_t(i)];
            nodeRows.push_back({i, nodeTypeName(types[size_t(i)]), animation, guid});
            QJsonObject node{{"index", i}, {"type", nodeTypeName(types[size_t(i)])}, {"guid", guid}};
            if (!animation.isEmpty())
                node.insert("animation", animation);
            nodeJson.append(node);
            if (!animation.isEmpty() && !animation.startsWith(u'('))
                ++uses[animation];
        }
        for (auto it = uses.begin(); it != uses.end(); ++it) {
            animationRows.push_back({it.key(), it.value()});
            animationJson.append(QJsonObject{{"name", it.key()}, {"nodes", it.value()}});
        }
    } catch (const ParseFailure &f) {
        if (error)
            *error = QString::fromStdString(f.what());
        return nullptr;
    }

    const QJsonObject json{{"name", name},
                           {"version", QStringLiteral("0x%1").arg(version, 0, 16)},
                           {"commands", commandJson},
                           {"animations", animationJson},
                           {"nodes", nodeJson}};
    const auto exporter = [json] { return QJsonDocument(json).toJson(); };
    auto doc = std::make_unique<Document>();
    const int commandCount = int(commandRows.size()), nodeCount = int(nodeRows.size()), animationCount = int(animationRows.size());
    auto commands = std::make_shared<SimpleTable>(
        QObject::tr("Commands"),
        std::vector<SimpleColumn>{{QObject::tr("command")}, {QObject::tr("first node")}, {QObject::tr("GUID")}},
        std::move(commandRows));
    commands->setDescription(QObject::tr("%1, version 0x%2: %n command(s)", nullptr, commandCount).arg(name).arg(version, 0, 16));
    commands->setJsonExporter(exporter);
    auto animations = std::make_shared<SimpleTable>(
        QObject::tr("Animations"), std::vector<SimpleColumn>{{QObject::tr("animation")}, {QObject::tr("nodes")}},
        std::move(animationRows));
    animations->setDescription(
        QObject::tr("%n skeletal or bone animation(s) played; they are in the actor's .anim.bfres files", nullptr, animationCount));
    animations->setJsonExporter(exporter);
    auto nodes = std::make_shared<SimpleTable>(
        QObject::tr("Nodes"),
        std::vector<SimpleColumn>{{QObject::tr("index")}, {QObject::tr("node")}, {QObject::tr("animation")}, {QObject::tr("GUID")}},
        std::move(nodeRows));
    nodes->setDescription(QObject::tr("%n node(s)", nullptr, nodeCount));
    nodes->setJsonExporter(exporter);
    doc->tables.push_back(std::move(commands));
    doc->tables.push_back(std::move(animations));
    doc->tables.push_back(std::move(nodes));
    return doc;
}
