#include "data/baevreader.h"

#include "archives/decompress.h"
#include "archives/zstddictionaries.h"
#include "core/simpletable.h"
#include "models/bfresreader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QtEndian>

#include <bit>
#include <stdexcept>
#include <vector>

namespace {

constexpr int kMaxItems = 1 << 20;
constexpr qint64 kMaxBfres = qint64{256} * 1024 * 1024;

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
    quint32 u32(qint64 at) const
    {
        need(at, 4);
        return qFromLittleEndian<quint32>(m_data.constData() + at);
    }
    float f32(qint64 at) const { return std::bit_cast<float>(u32(at)); }
    qint64 u64(qint64 at) const
    {
        need(at, 8);
        const quint64 v = qFromLittleEndian<quint64>(m_data.constData() + at);
        if (v > quint64(m_data.size()))
            throw Failure(QObject::tr("A pointer points outside the file.").toStdString());
        return qint64(v);
    }
    struct Array
    {
        qint64 at = 0;
        int count = 0;
        qint64 stride = 0;
    };
    Array array(qint64 at) const
    {
        Array a{u64(at), int(u32(at + 8)), qint64(u32(at + 12))};
        if (a.count < 0 || a.count > kMaxItems || (a.count > 0 && a.stride <= 0))
            throw Failure(QObject::tr("The file lists an impossible number of items.").toStdString());
        need(a.at, qint64(a.count) * a.stride);
        return a;
    }
    QString cstr(qint64 at) const
    {
        need(at, 1);
        const char *p = m_data.constData() + at;
        return QString::fromUtf8(p, qsizetype(qstrnlen(p, uint(m_data.size() - at))));
    }

private:
    const QByteArray &m_data;
};

struct Param
{
    QString text;
    QJsonValue json;
};

Param readParam(const Reader &r, qint64 at)
{
    const quint32 type = r.u32(at);
    switch (type) {
    case 0: {
        const qint32 v = qint32(r.u32(at + 8));
        return {QString::number(v), v};
    }
    case 1: {
        const float v = r.f32(at + 8);
        return {QString::number(v, 'g', 7), double(v)};
    }
    case 3: {
        const float x = r.f32(at + 8), y = r.f32(at + 12), z = r.f32(at + 16);
        return {QStringLiteral("(%1, %2, %3)").arg(x).arg(y).arg(z), QJsonArray{double(x), double(y), double(z)}};
    }
    case 5: {
        const QString v = r.cstr(r.u64(at + 8));
        return {u'"' + v + u'"', v};
    }
    default: return {QObject::tr("(type %1)").arg(type), QJsonValue()};
    }
}

QHash<quint32, QString> clipNamesFor(const QString &fileName)
{
    QHash<quint32, QString> names;
    QString base = QFileInfo(fileName).fileName();
    if (base.endsWith(QLatin1String(".zs")))
        base.chop(3);
    if (!base.endsWith(QLatin1String(".anim.baev")))
        return names;
    base.chop(5);
    for (const QString &root : zstdDictionaryRoots()) {
        for (const QString &candidate : {base + QStringLiteral(".bfres.zs"), base + QStringLiteral(".bfres")}) {
            QFile file(QDir(root).filePath(QStringLiteral("Model/") + candidate));
            if (file.size() > kMaxBfres || !file.open(QIODevice::ReadOnly))
                continue;
            const std::optional<QByteArray> bytes = decompress(file.readAll(), nullptr);
            if (!bytes)
                continue;
            for (const BfresClip &clip : BfresReader::skeletalClips(*bytes))
                names.insert(BaevReader::fnv1a(clip.name.toUtf8()), clip.name);
            return names;
        }
    }
    return names;
}

}

quint32 BaevReader::fnv1a(QByteArrayView text)
{
    quint32 h = 0x811C9DC5u;
    for (const char c : text)
        h = (h ^ uchar(c)) * 0x01000193u;
    return h;
}

int BaevReader::score(const QString &fileName, QByteArrayView head) const
{
    if (!head.startsWith("BFFH"))
        return 0;
    return head.size() >= 0xA8 && head.sliced(0x28).startsWith("Nintendo.AnimationEvent") ? 100
         : fileName.contains(QLatin1String(".baev")) ? 90 : 0;
}

std::unique_ptr<Document> BaevReader::read(const QString &path, QString *error) const
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
    loadZstdDictionariesNear(path);
    return readBytes(file.readAll(), path, error);
}

std::unique_ptr<Document> BaevReader::readBytes(const QByteArray &data, const QString &fileName, QString *error) const
{
    return readWithNames(data, clipNamesFor(fileName), error);
}

std::unique_ptr<Document> BaevReader::readWithNames(const QByteArray &data, const QHash<quint32, QString> &names,
                                                    QString *error)
{
    std::vector<SimpleTable::Row> rows;
    QJsonArray animationJson;
    QString version;
    int resolved = 0, keys = 0;
    try {
        const Reader r(data);
        if (data.size() < 0xA8 || !data.startsWith("BFFH"))
            throw Failure(QObject::tr("Not an animation event file (no \"BFFH\" at the start).").toStdString());
        const Reader::Array sections = r.array(0x10);
        if (sections.count < 1)
            throw Failure(QObject::tr("The file has no sections.").toStdString());
        const qint64 base = r.u64(sections.at + 0x10);
        r.need(base, 0x38);
        version = QStringLiteral("%1.%2.%3")
                      .arg(qFromLittleEndian<quint16>(data.constData() + base + 10))
                      .arg(quint8(data[base + 9]))
                      .arg(quint8(data[base + 8]));
        const Reader::Array infos = r.array(base + 0x18);
        const Reader::Array nodes = r.array(base + 0x28);
        keys = infos.count;
        for (int i = 0; i < infos.count; ++i) {
            const qint64 info = infos.at + qint64(i) * infos.stride;
            const quint32 hash = r.u32(info);
            const auto found = names.constFind(hash);
            const QString animation = found != names.cend() ? *found : QStringLiteral("0x%1").arg(hash, 8, 16, QLatin1Char('0'));
            if (found != names.cend())
                ++resolved;
            const Reader::Array indices = r.array(info + 8);
            QJsonArray eventJson;
            for (int k = 0; k < indices.count; ++k) {
                const quint32 index = r.u32(indices.at + qint64(k) * 4);
                if (index >= quint32(nodes.count))
                    throw Failure(QObject::tr("An event list points past the end of the event table.").toStdString());
                const qint64 node = nodes.at + qint64(index) * nodes.stride;
                const Reader::Array events = r.array(node);
                for (int e = 0; e < events.count; ++e) {
                    const qint64 ev = events.at + qint64(e) * events.stride;
                    const QString name = r.cstr(r.u64(ev));
                    for (const bool hold : {false, true}) {
                        const Reader::Array list = r.array(ev + (hold ? 0x18 : 0x08));
                        for (int t = 0; t < list.count; ++t) {
                            const qint64 entry = list.at + qint64(t) * list.stride;
                            const Reader::Array params = r.array(entry);
                            QStringList texts;
                            QJsonArray paramJson;
                            for (int p = 0; p < params.count; ++p) {
                                const Param param = readParam(r, r.u64(params.at + qint64(p) * params.stride));
                                texts << param.text;
                                paramJson.append(param.json);
                            }
                            const float start = r.f32(entry + 0x10);
                            const QVariant end = hold ? QVariant(double(r.f32(entry + 0x14))) : QVariant();
                            rows.push_back({animation, name, hold ? QObject::tr("hold") : QObject::tr("trigger"), double(start),
                                            end, texts.join(QStringLiteral(", "))});
                            QJsonObject event{{"name", name}, {"kind", hold ? "hold" : "trigger"}, {"start", double(start)},
                                              {"params", paramJson}};
                            if (hold)
                                event.insert("end", end.toDouble());
                            eventJson.append(event);
                        }
                    }
                }
            }
            animationJson.append(QJsonObject{{"key", animation}, {"hash", QStringLiteral("0x%1").arg(hash, 8, 16, QLatin1Char('0'))},
                                             {"events", eventJson}});
        }
    } catch (const Failure &f) {
        if (error)
            *error = QString::fromStdString(f.what());
        return nullptr;
    }
    const QJsonObject json{{"version", version}, {"animations", animationJson}};
    const int count = int(rows.size());
    auto table = std::make_shared<SimpleTable>(
        QObject::tr("Animation events"),
        std::vector<SimpleColumn>{{QObject::tr("animation / node")},
                                  {QObject::tr("event")},
                                  {QObject::tr("kind")},
                                  {QObject::tr("start frame")},
                                  {QObject::tr("end frame")},
                                  {QObject::tr("parameters")}},
        std::move(rows));
    QString description = QObject::tr("Version %1: %n event(s)", nullptr, count).arg(version);
    if (keys > 0)
        description += resolved == keys ? QObject::tr(", all animations named")
                     : resolved > 0    ? QObject::tr(", %1 of %2 animations named").arg(resolved).arg(keys)
                                       : QObject::tr(", keyed by hashes (FNV-1a of the animation name or ASB node)");
    table->setDescription(description);
    table->setJsonExporter([json] { return QJsonDocument(json).toJson(); });
    auto doc = std::make_unique<Document>();
    doc->tables.push_back(std::move(table));
    return doc;
}
