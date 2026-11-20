#include "data/ainbreader.h"

#include "core/simpletable.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>
#include <vector>

namespace {

constexpr qsizetype kHeaderSize = 0x74;
constexpr qsizetype kCommandSize = 0x18;
constexpr qsizetype kNodeSize = 0x3C;
constexpr qsizetype kParamMetaSize = 0xA4;
constexpr int kValueTypes = 6;
constexpr int kLinkTypes = 10;
constexpr int kMaxItems = 1 << 20;

enum ValueType { Int, Bool, Float, String, Vec3f, UserDefined };

struct Failure : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

class Reader
{
public:
    explicit Reader(const QByteArray &data) : m_data(data) {}

    void need(qint64 at, qint64 size) const
    {
        if (at < 0 || size < 0 || at > m_data.size() - size)
            throw Failure(QObject::tr("An offset points outside the file.").toStdString());
    }
    quint8 u8(qint64 at) const
    {
        need(at, 1);
        return quint8(m_data[at]);
    }
    quint16 u16(qint64 at) const
    {
        need(at, 2);
        return qFromLittleEndian<quint16>(m_data.constData() + at);
    }
    qint16 s16(qint64 at) const { return qint16(u16(at)); }
    quint32 u32(qint64 at) const
    {
        need(at, 4);
        return qFromLittleEndian<quint32>(m_data.constData() + at);
    }
    float f32(qint64 at) const { return std::bit_cast<float>(u32(at)); }
    int count(quint32 n) const
    {
        if (n > quint32(kMaxItems))
            throw Failure(QObject::tr("The file lists an impossible number of items.").toStdString());
        return int(n);
    }

    void setPool(qint64 at) { m_pool = at; }
    QString str(quint32 offset) const
    {
        const qint64 at = m_pool + offset;
        need(at, 1);
        const char *p = m_data.constData() + at;
        return QString::fromUtf8(p, qsizetype(qstrnlen(p, uint(m_data.size() - at))));
    }
    QString guid(qint64 at) const
    {
        need(at, 16);
        const QByteArray b = m_data.mid(at, 16);
        return QStringLiteral("%1-%2-%3-%4-%5")
            .arg(u32(at), 8, 16, QLatin1Char('0'))
            .arg(u16(at + 4), 4, 16, QLatin1Char('0'))
            .arg(u16(at + 6), 4, 16, QLatin1Char('0'))
            .arg(u16(at + 8), 4, 16, QLatin1Char('0'))
            .arg(QString::fromLatin1(b.mid(10).toHex()));
    }

private:
    const QByteArray &m_data;
    qint64 m_pool = 0;
};

QString typeName(int type)
{
    static const std::array<const char *, kValueTypes> names{"int", "bool", "float", "string", "vec3f", "user-defined"};
    return QString::fromLatin1(names[size_t(type)]);
}

QString nodeTypeName(int type)
{
    switch (type) {
    case 1: return QStringLiteral("S32Selector");
    case 2: return QStringLiteral("Sequential");
    case 3: return QStringLiteral("Simultaneous");
    case 4: return QStringLiteral("F32Selector");
    case 5: return QStringLiteral("StringSelector");
    case 6: return QStringLiteral("RandomSelector");
    case 7: return QStringLiteral("BoolSelector");
    case 8: return QStringLiteral("Fork");
    case 9: return QStringLiteral("Join");
    case 10: return QStringLiteral("Alert");
    case 20: return QStringLiteral("Expression");
    case 100: return QStringLiteral("ModuleIF_Input_S32");
    case 101: return QStringLiteral("ModuleIF_Input_F32");
    case 102: return QStringLiteral("ModuleIF_Input_Vec3f");
    case 103: return QStringLiteral("ModuleIF_Input_String");
    case 104: return QStringLiteral("ModuleIF_Input_Bool");
    case 105: return QStringLiteral("ModuleIF_Input_Ptr");
    case 200: return QStringLiteral("ModuleIF_Output_S32");
    case 201: return QStringLiteral("ModuleIF_Output_F32");
    case 202: return QStringLiteral("ModuleIF_Output_Vec3f");
    case 203: return QStringLiteral("ModuleIF_Output_String");
    case 204: return QStringLiteral("ModuleIF_Output_Bool");
    case 205: return QStringLiteral("ModuleIF_Output_Ptr");
    case 300: return QStringLiteral("ModuleIF_Child");
    case 400: return QStringLiteral("StateEnd");
    case 500: return QStringLiteral("SplitTiming");
    default: return QStringLiteral("type %1").arg(type);
    }
}

QString linkTypeName(int type)
{
    switch (type) {
    case 0: return QStringLiteral("generic");
    case 2: return QStringLiteral("child");
    case 3: return QStringLiteral("transition");
    case 4: return QStringLiteral("string");
    case 5: return QStringLiteral("int");
    default: return QStringLiteral("plug %1").arg(type);
    }
}

struct Value
{
    QString text;
    QJsonValue json;
};

Value readValue(const Reader &r, int type, qint64 &at)
{
    switch (type) {
    case Int: {
        const qint32 v = qint32(r.u32(at));
        at += 4;
        return {QString::number(v), v};
    }
    case Bool: {
        const bool v = r.u32(at) != 0;
        at += 4;
        return {v ? QStringLiteral("true") : QStringLiteral("false"), v};
    }
    case Float: {
        const float v = r.f32(at);
        at += 4;
        return {QString::number(v, 'g', 7), double(v)};
    }
    case String: {
        const QString v = r.str(r.u32(at));
        at += 4;
        return {u'"' + v + u'"', v};
    }
    case Vec3f: {
        const float x = r.f32(at), y = r.f32(at + 4), z = r.f32(at + 8);
        at += 12;
        return {QStringLiteral("(%1, %2, %3)").arg(x).arg(y).arg(z), QJsonArray{double(x), double(y), double(z)}};
    }
    default: return {};
    }
}

struct Param
{
    QString name;
    QString className;
    Value value;
    std::vector<std::pair<int, int>> sources;
};

struct Ainb
{
    quint32 version = 0;
    QString name;
    QString category;
    std::vector<std::array<QString, 3>> commands;
    std::vector<SimpleTable::Row> globals;
    QJsonArray globalJson;
    std::vector<SimpleTable::Row> modules;
    QJsonArray moduleJson;
    std::vector<SimpleTable::Row> nodes;
    QJsonArray nodeJson;
    bool paramsRead = true;
};

std::array<std::vector<Param>, kValueTypes> readImmediates(const Reader &r, qint64 start, qint64 end)
{
    std::array<qint64, kValueTypes> stops{};
    for (int i = 1; i < kValueTypes; ++i)
        stops[size_t(i - 1)] = r.u32(start + i * 4);
    stops[kValueTypes - 1] = end;
    std::array<std::vector<Param>, kValueTypes> out;
    qint64 at = start + kValueTypes * 4;
    for (int t = 0; t < kValueTypes; ++t) {
        while (at < stops[size_t(t)]) {
            Param p;
            p.name = r.str(r.u32(at));
            at += 4;
            if (t == UserDefined) {
                p.className = r.str(r.u32(at));
                at += 4;
            }
            at += 4;
            p.value = readValue(r, t, at);
            out[size_t(t)].push_back(std::move(p));
            r.count(quint32(out[size_t(t)].size()));
        }
        if (at != stops[size_t(t)])
            throw Failure("immediate parameters do not line up");
    }
    return out;
}

struct IoParams
{
    std::array<std::vector<Param>, kValueTypes> inputs;
    std::array<std::vector<Param>, kValueTypes> outputs;
};

IoParams readIo(const Reader &r, qint64 start, qint64 end, const std::vector<std::pair<int, int>> &multi)
{
    std::array<qint64, kValueTypes * 2> stops{};
    for (int i = 1; i < kValueTypes * 2; ++i)
        stops[size_t(i - 1)] = r.u32(start + i * 4);
    stops[kValueTypes * 2 - 1] = end;
    IoParams out;
    qint64 at = start + kValueTypes * 2 * 4;
    for (int t = 0; t < kValueTypes; ++t) {
        while (at < stops[size_t(t * 2)]) {
            Param p;
            p.name = r.str(r.u32(at));
            at += 4;
            if (t == UserDefined) {
                p.className = r.str(r.u32(at));
                at += 4;
            }
            const qint16 node = r.s16(at);
            at += 2;
            if (node <= -100) {
                const int n = r.u16(at);
                at += 2 + 4;
                if (t == UserDefined)
                    at += 4;
                const int base = -node - 100;
                for (int k = 0; k < n; ++k)
                    if (size_t(base + k) < multi.size())
                        p.sources.push_back(multi[size_t(base + k)]);
            } else {
                const qint16 param = r.s16(at);
                at += 2 + 4;
                if (t == UserDefined)
                    at += 4;
                if (node >= 0)
                    p.sources.push_back({node, param});
            }
            p.value = readValue(r, t, at);
            out.inputs[size_t(t)].push_back(std::move(p));
            r.count(quint32(out.inputs[size_t(t)].size()));
        }
        if (at != stops[size_t(t * 2)])
            throw Failure("input parameters do not line up");
        while (at < stops[size_t(t * 2 + 1)]) {
            Param p;
            p.name = r.str(r.u32(at) & 0x7FFFFFFF);
            at += 4;
            if (t == UserDefined) {
                p.className = r.str(r.u32(at));
                at += 4;
            }
            out.outputs[size_t(t)].push_back(std::move(p));
            r.count(quint32(out.outputs[size_t(t)].size()));
        }
        if (at != stops[size_t(t * 2 + 1)])
            throw Failure("output parameters do not line up");
    }
    return out;
}

void readGlobals(const Reader &r, qint64 at, Ainb &out)
{
    constexpr int kUnknown = -1;
    static const std::vector<int> older{String, Int, Float, Bool, Vec3f, UserDefined};
    static const std::vector<int> newer{String, Int, kUnknown, Float, Bool, Vec3f, UserDefined};
    const std::vector<int> &typeOf = out.version >= 0x408 ? newer : older;
    const int slotCount = int(typeOf.size());
    std::vector<int> counts(static_cast<size_t>(slotCount), 0);
    std::vector<int> offsets(static_cast<size_t>(slotCount), 0);
    for (int i = 0; i < slotCount; ++i) {
        counts[size_t(i)] = r.count(r.u16(at + i * 8));
        offsets[size_t(i)] = r.u16(at + i * 8 + 4);
    }
    qint64 p = at + slotCount * 8;
    struct Global
    {
        QString name;
        QString notes;
        int type;
        int slot;
        bool fileRef;
        Value value;
        QString file;
    };
    std::vector<Global> globals;
    for (int i = 0; i < slotCount; ++i)
        for (int k = 0; k < counts[size_t(i)]; ++k) {
            const quint32 nameAndFlags = r.u32(p);
            globals.push_back({r.str(nameAndFlags & 0x3FFFFF), r.str(r.u32(p + 4)), typeOf[size_t(i)], i,
                               ((nameAndFlags >> 23) & 1) == 0, {}, {}});
            p += 8;
        }
    for (Global &g : globals) {
        if (g.type == UserDefined)
            continue;
        if (g.type == kUnknown) {
            qint64 size = 4;
            for (int next = g.slot + 1; next < slotCount; ++next)
                if (offsets[size_t(next)] > offsets[size_t(g.slot)]) {
                    size = (offsets[size_t(next)] - offsets[size_t(g.slot)]) / std::max(1, counts[size_t(g.slot)]);
                    break;
                }
            r.need(p, size);
            g.value = {QStringLiteral("0x%1").arg(QString::fromLatin1(QByteArray::number(qulonglong(r.u32(p)), 16))), {}};
            p += size;
            continue;
        }
        g.value = readValue(r, g.type, p);
    }
    for (Global &g : globals)
        if (g.fileRef && out.version < 0x408) {
            g.file = r.str(r.u32(p));
            p += 16;
        }
    for (const Global &g : globals) {
        const QString type = g.type < 0 ? QStringLiteral("type %1").arg(g.slot) : typeName(g.type);
        out.globals.push_back({g.name, type, g.value.text, g.notes, g.file});
        out.globalJson.append(QJsonObject{{"name", g.name},
                                          {"type", type},
                                          {"default", g.value.json},
                                          {"notes", g.notes},
                                          {"file", g.file}});
    }
}

Ainb parseAinb(const QByteArray &data)
{
    Reader r(data);
    if (data.size() < kHeaderSize || !data.startsWith("AIB "))
        throw Failure(QObject::tr("Not an AINB file (no \"AIB \" at the start).").toStdString());
    Ainb a;
    a.version = r.u32(4);
    r.setPool(r.u32(0x24));
    a.name = r.str(r.u32(0x08));
    a.category = r.str(r.u32(0x60));
    const int commands = r.count(r.u32(0x0C));
    const int nodes = r.count(r.u32(0x10));

    std::vector<int> preconditions;
    const qint64 preStart = r.u32(0x4C);
    const qint64 preEnd = r.u32(0x44) ? r.u32(0x44) : r.u32(0x5C);
    for (qint64 at = preStart; at + 4 <= preEnd && preconditions.size() < size_t(kMaxItems); at += 4)
        preconditions.push_back(r.u16(at));

    std::vector<std::pair<int, int>> multi;
    for (qint64 at = r.u32(0x38); at + 8 <= qint64(r.u32(0x30)) && multi.size() < size_t(kMaxItems); at += 8)
        multi.push_back({r.u16(at), r.u16(at + 2)});

    std::array<std::vector<Param>, kValueTypes> immediates;
    IoParams io;
    try {
        immediates = readImmediates(r, r.u32(0x2C), r.u32(0x34));
        io = readIo(r, r.u32(0x34), r.u32(0x38), multi);
    } catch (const Failure &) {
        a.paramsRead = false;
        immediates = {};
        io = {};
    }

    readGlobals(r, r.u32(0x20), a);

    const qint64 embedded = r.u32(0x5C);
    if (embedded) {
        const int n = r.count(r.u32(embedded));
        for (int i = 0; i < n; ++i) {
            const qint64 e = embedded + 4 + qint64(i) * 12;
            const QString name = r.str(r.u32(e)), category = r.str(r.u32(e + 4));
            const int uses = int(r.u32(e + 8));
            a.modules.push_back({name, category, uses});
            a.moduleJson.append(QJsonObject{{"name", name}, {"category", category}, {"count", uses}});
        }
    }

    const qint64 nodeStart = kHeaderSize + qint64(commands) * kCommandSize;
    std::vector<QString> nodeLabels;
    for (int i = 0; i < nodes; ++i) {
        const qint64 n = nodeStart + qint64(i) * kNodeSize;
        const int type = r.u16(n);
        nodeLabels.push_back(type == 0 ? r.str(r.u32(n + 8)) : nodeTypeName(type));
    }
    auto nodeRef = [&](int index) {
        return index >= 0 && size_t(index) < nodeLabels.size() ? QStringLiteral("#%1 %2").arg(index).arg(nodeLabels[size_t(index)])
                                                                : QStringLiteral("#%1").arg(index);
    };

    for (int i = 0; i < commands; ++i) {
        const qint64 c = kHeaderSize + qint64(i) * kCommandSize;
        a.commands.push_back({r.str(r.u32(c)), nodeRef(r.u16(c + 0x14)), r.guid(c + 4)});
    }

    for (int i = 0; i < nodes; ++i) {
        const qint64 n = nodeStart + qint64(i) * kNodeSize;
        const int type = r.u16(n);
        const qint64 meta = r.u32(n + 0x14);
        QStringList immText, inText, outText, linkText;
        QJsonObject immJson;
        QJsonArray inJson, outJson, linkJson;
        r.need(meta, kParamMetaSize);
        for (int t = 0; t < kValueTypes && a.paramsRead; ++t) {
            const quint32 immOffset = r.u32(meta + t * 8), immCount = r.u32(meta + t * 8 + 4);
            for (quint32 k = 0; k < immCount && immOffset + k < immediates[size_t(t)].size(); ++k) {
                const Param &p = immediates[size_t(t)][immOffset + k];
                immText << p.name + u'=' + (t == UserDefined ? p.className : p.value.text);
                immJson.insert(p.name, t == UserDefined ? QJsonValue(p.className) : p.value.json);
            }
            const qint64 io4 = meta + 48 + t * 16;
            const quint32 inOffset = r.u32(io4), inCount = r.u32(io4 + 4);
            const quint32 outOffset = r.u32(io4 + 8), outCount = r.u32(io4 + 12);
            for (quint32 k = 0; k < inCount && inOffset + k < io.inputs[size_t(t)].size(); ++k) {
                const Param &p = io.inputs[size_t(t)][inOffset + k];
                QStringList from;
                QJsonArray fromJson;
                for (const auto &[node, param] : p.sources) {
                    from << QStringLiteral("%1.%2").arg(nodeRef(node)).arg(param);
                    fromJson.append(QJsonObject{{"node", node}, {"param", param}});
                }
                inText << p.name + (from.isEmpty() ? u'=' + p.value.text : QStringLiteral(" ← ") + from.join(QStringLiteral(", ")));
                inJson.append(QJsonObject{{"name", p.name}, {"type", typeName(t)}, {"default", p.value.json}, {"from", fromJson}});
            }
            for (quint32 k = 0; k < outCount && outOffset + k < io.outputs[size_t(t)].size(); ++k) {
                const Param &p = io.outputs[size_t(t)][outOffset + k];
                outText << p.name;
                outJson.append(QJsonObject{{"name", p.name}, {"type", typeName(t)}});
            }
        }
        qint64 linkAt = meta + kParamMetaSize - kLinkTypes * 2;
        qint64 offsets = meta + kParamMetaSize;
        for (int t = 0; t < kLinkTypes; ++t) {
            const int count = r.u8(linkAt + t * 2);
            for (int k = 0; k < count; ++k) {
                const qint64 link = r.u32(offsets);
                offsets += 4;
                const int target = int(r.u32(link));
                const bool labelled = t == 0 || t == 2 || t == 4 || t == 5;
                const QString label = labelled ? r.str(r.u32(link + 4)) : QString();
                linkText << linkTypeName(t) + QStringLiteral(" → ") + nodeRef(target)
                                + (label.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(label));
                linkJson.append(QJsonObject{{"kind", linkTypeName(t)}, {"node", target}, {"label", label}});
            }
        }
        QStringList pre;
        const int preBase = r.u16(n + 0x24), preCount = r.u16(n + 0x26);
        for (int k = 0; k < preCount && size_t(preBase + k) < preconditions.size(); ++k)
            pre << nodeRef(preconditions[size_t(preBase + k)]);
        const QString guid = r.guid(n + 0x2C);
        const QString semicolon = QStringLiteral("; ");
        a.nodes.push_back({i, nodeLabels[size_t(i)], immText.join(semicolon), inText.join(semicolon),
                           outText.join(semicolon), linkText.join(semicolon), pre.join(semicolon), guid});
        a.nodeJson.append(QJsonObject{{"index", i},
                                      {"type", type == 0 ? QStringLiteral("UserDefined") : nodeTypeName(type)},
                                      {"name", type == 0 ? nodeLabels[size_t(i)] : QString()},
                                      {"guid", guid},
                                      {"flags", int(r.u8(n + 6))},
                                      {"parameters", immJson},
                                      {"inputs", inJson},
                                      {"outputs", outJson},
                                      {"links", linkJson}});
    }
    return a;
}

}

int AinbReader::score(const QString &, QByteArrayView head) const
{
    return head.startsWith("AIB ") ? 100 : 0;
}

std::unique_ptr<Document> AinbReader::read(const QString &path, QString *error) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return nullptr;
    }
    if (file.size() > kMaxFileSize) {
        if (error)
            *error = QObject::tr("File is larger than 64 MB.");
        return nullptr;
    }
    return readBytes(file.readAll(), path, error);
}

std::unique_ptr<Document> AinbReader::readBytes(const QByteArray &data, const QString &fileName, QString *error) const
{
    Ainb a;
    try {
        a = parseAinb(data);
    } catch (const Failure &f) {
        if (error)
            *error = QString::fromStdString(f.what());
        return nullptr;
    }
    QJsonArray commandJson;
    std::vector<SimpleTable::Row> commandRows;
    for (const auto &c : a.commands) {
        commandRows.push_back({c[0], c[1], c[2]});
        commandJson.append(QJsonObject{{"name", c[0]}, {"root", c[1]}, {"guid", c[2]}});
    }
    const QJsonObject json{{"name", a.name.isEmpty() ? QFileInfo(fileName).fileName() : a.name},
                           {"category", a.category},
                           {"version", QStringLiteral("0x%1").arg(a.version, 0, 16)},
                           {"commands", commandJson},
                           {"globals", a.globalJson},
                           {"modules", a.moduleJson},
                           {"nodes", a.nodeJson}};
    const auto exporter = [json] { return QJsonDocument(json).toJson(); };
    const int nodeCount = int(a.nodes.size());
    auto doc = std::make_unique<Document>();
    auto nodes = std::make_shared<SimpleTable>(
        QObject::tr("Nodes"),
        std::vector<SimpleColumn>{{QObject::tr("index")},
                                  {QObject::tr("node")},
                                  {QObject::tr("parameters")},
                                  {QObject::tr("inputs")},
                                  {QObject::tr("outputs")},
                                  {QObject::tr("links")},
                                  {QObject::tr("preconditions")},
                                  {QObject::tr("GUID")}},
        std::move(a.nodes));
    QString description = QObject::tr("%1 AI, version 0x%2: %n node(s)", nullptr, nodeCount)
                              .arg(a.category.isEmpty() ? QObject::tr("Unnamed") : a.category)
                              .arg(a.version, 0, 16);
    if (!a.paramsRead)
        description += QObject::tr(" (their parameters use a layout this app does not know yet)");
    nodes->setDescription(description);
    nodes->setJsonExporter(exporter);
    doc->tables.push_back(std::move(nodes));
    if (!commandRows.empty()) {
        auto table = std::make_shared<SimpleTable>(
            QObject::tr("Commands"),
            std::vector<SimpleColumn>{{QObject::tr("name")}, {QObject::tr("first node")}, {QObject::tr("GUID")}},
            std::move(commandRows));
        table->setDescription(QObject::tr("Entry points of this AI"));
        table->setJsonExporter(exporter);
        doc->tables.push_back(std::move(table));
    }
    if (!a.globals.empty()) {
        auto table = std::make_shared<SimpleTable>(
            QObject::tr("Global parameters"),
            std::vector<SimpleColumn>{{QObject::tr("name")}, {QObject::tr("type")}, {QObject::tr("default")},
                                      {QObject::tr("notes")}, {QObject::tr("file")}},
            std::move(a.globals));
        table->setJsonExporter(exporter);
        doc->tables.push_back(std::move(table));
    }
    if (!a.modules.empty()) {
        auto table = std::make_shared<SimpleTable>(
            QObject::tr("Modules"),
            std::vector<SimpleColumn>{{QObject::tr("module")}, {QObject::tr("category")}, {QObject::tr("uses")}},
            std::move(a.modules));
        table->setDescription(QObject::tr("Other AINB files this one runs"));
        table->setJsonExporter(exporter);
        doc->tables.push_back(std::move(table));
    }
    return doc;
}
