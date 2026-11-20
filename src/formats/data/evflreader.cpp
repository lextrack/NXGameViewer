#include "data/evflreader.h"

#include "core/simpletable.h"

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
constexpr quint16 kNone = 0xFFFF;

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
    quint32 u32(qint64 at) const
    {
        need(at, 4);
        return qFromLittleEndian<quint32>(m_data.constData() + at);
    }
    qint64 ptr(qint64 at) const
    {
        need(at, 8);
        const quint64 v = qFromLittleEndian<quint64>(m_data.constData() + at);
        if (v > quint64(m_data.size()))
            throw Failure(QObject::tr("A pointer points outside the file.").toStdString());
        return qint64(v);
    }
    int count(quint32 n) const
    {
        if (n > quint32(kMaxItems))
            throw Failure(QObject::tr("The file lists an impossible number of items.").toStdString());
        return int(n);
    }
    QString str(qint64 at) const
    {
        const qint64 p = ptr(at);
        if (p == 0)
            return {};
        const quint16 n = u16(p);
        need(p + 2, n);
        return QString::fromUtf8(m_data.constData() + p + 2, n);
    }
    QStringList dic(qint64 at) const
    {
        QStringList names;
        if (at == 0)
            return names;
        const int n = count(u32(at + 4));
        for (int i = 0; i < n; ++i)
            names << str(at + 0x20 + qint64(i) * 0x10);
        return names;
    }

private:
    const QByteArray &m_data;
};

struct Item
{
    QString text;
    QJsonValue json;
};

Item containerItem(const Reader &r, qint64 at)
{
    const int type = r.u8(at);
    const int n = r.count(r.u16(at + 2));
    const qint64 v = at + 0x10;
    auto list = [&](auto make, int stride) {
        QStringList texts;
        QJsonArray json;
        for (int i = 0; i < n; ++i) {
            const Item item = make(v + qint64(i) * stride);
            texts << item.text;
            json.append(item.json);
        }
        return Item{u'[' + texts.join(QStringLiteral(", ")) + u']', json};
    };
    auto intItem = [&](qint64 p) {
        const qint32 x = qint32(r.u32(p));
        return Item{QString::number(x), x};
    };
    auto boolItem = [&](qint64 p) {
        const bool x = r.u32(p) != 0;
        return Item{x ? QStringLiteral("true") : QStringLiteral("false"), x};
    };
    auto floatItem = [&](qint64 p) {
        const float x = std::bit_cast<float>(r.u32(p));
        return Item{QString::number(x, 'g', 7), double(x)};
    };
    auto stringItem = [&](qint64 p) {
        const QString x = r.str(p);
        return Item{u'"' + x + u'"', x};
    };
    switch (type) {
    case 0: {
        const QString x = r.str(v);
        return {QStringLiteral("argument %1").arg(x), QJsonObject{{"argument", x}}};
    }
    case 2: return intItem(v);
    case 3: return boolItem(v);
    case 4: return floatItem(v);
    case 5: return stringItem(v);
    case 7: return list(intItem, 4);
    case 8: return list(boolItem, 4);
    case 9: return list(floatItem, 4);
    case 10: return list(stringItem, 8);
    case 12:
    case 13: {
        const QString a = r.str(v), b = r.str(v + 8);
        const QString text = b.isEmpty() ? a : a + u'/' + b;
        return {text, type == 12 ? QJsonObject{{"actor", a}, {"sub", b}} : QJsonObject{{"name", a}, {"key", b}}};
    }
    default: return {QObject::tr("(type %1)").arg(type), QJsonValue()};
    }
}

Item container(const Reader &r, qint64 at)
{
    if (at == 0)
        return {{}, QJsonObject()};
    if (r.u8(at) != 1)
        throw Failure(QObject::tr("A parameter list has the wrong type.").toStdString());
    const QStringList names = r.dic(r.ptr(at + 8));
    QStringList texts;
    QJsonObject json;
    for (int i = 0; i < names.size(); ++i) {
        const Item item = containerItem(r, r.ptr(at + 0x10 + qint64(i) * 8));
        texts << names[i] + u'=' + item.text;
        json.insert(names[i], item.json);
    }
    return {texts.join(QStringLiteral("; ")), json};
}

struct Actor
{
    QString name;
    QStringList actions;
    QStringList queries;
};

struct Flow
{
    QString name;
    quint16 version = 0;
    bool hasChart = false;
    std::vector<SimpleTable::Row> events;
    std::vector<SimpleTable::Row> entries;
    std::vector<SimpleTable::Row> actors;
    bool hasTimeline = false;
    QString timelineName;
    float duration = 0;
    std::vector<SimpleTable::Row> clips;
    std::vector<SimpleTable::Row> oneshots;
    std::vector<SimpleTable::Row> cuts;
    std::vector<SimpleTable::Row> timelineActors;
    QStringList subtimelines;
    QJsonObject json;
};

qint64 actorStride(int count, qint64 actorsAt, qint64 nextAt)
{
    if (count > 0 && nextAt > actorsAt && (nextAt - actorsAt) % count == 0 && (nextAt - actorsAt) / count >= 0x38
        && (nextAt - actorsAt) / count <= 0x60)
        return (nextAt - actorsAt) / count;
    return 0x38;
}

std::vector<Actor> readActors(const Reader &r, qint64 actorsAt, int actorCount, qint64 actorSize,
                              std::vector<SimpleTable::Row> &rows, QJsonArray &actorJson)
{
    std::vector<Actor> actors;
    for (int i = 0; i < actorCount; ++i) {
        const qint64 a = actorsAt + qint64(i) * actorSize;
        Actor actor;
        const QString main = r.str(a), sub = r.str(a + 8);
        actor.name = sub.isEmpty() ? main : main + u'/' + sub;
        const qint64 actions = r.ptr(a + 0x18), queries = r.ptr(a + 0x20);
        const Item params = container(r, r.ptr(a + 0x28));
        const int actionCount = r.count(r.u16(a + 0x30)), queryCount = r.count(r.u16(a + 0x32));
        for (int k = 0; k < actionCount; ++k)
            actor.actions << r.str(actions + qint64(k) * 8);
        for (int k = 0; k < queryCount; ++k)
            actor.queries << r.str(queries + qint64(k) * 8);
        const QString argument = r.str(a + 0x10);
        rows.push_back({actor.name, argument, actor.actions.join(QStringLiteral(", ")),
                        actor.queries.join(QStringLiteral(", ")), params.text});
        actorJson.append(QJsonObject{{"name", main},
                                     {"sub_name", sub},
                                     {"argument", argument},
                                     {"actions", QJsonArray::fromStringList(actor.actions)},
                                     {"queries", QJsonArray::fromStringList(actor.queries)},
                                     {"params", params.json}});
        actors.push_back(std::move(actor));
    }
    return actors;
}

QString actorAction(const std::vector<Actor> &actors, quint16 actor, quint16 action)
{
    if (actor >= actors.size())
        return QStringLiteral("#%1").arg(actor);
    const Actor &a = actors[actor];
    return a.name + QStringLiteral(" · ") + (action < a.actions.size() ? a.actions[action] : QStringLiteral("#%1").arg(action));
}

void readTimeline(const Reader &r, qint64 tl, Flow &f)
{
    if (r.u32(tl) != 0x4E494C54u)
        throw Failure(QObject::tr("The timeline is missing.").toStdString());
    f.hasTimeline = true;
    f.duration = std::bit_cast<float>(r.u32(tl + 0x10));
    const int actorCount = r.count(r.u16(tl + 0x14));
    const int clipCount = r.count(r.u16(tl + 0x18));
    const int oneshotCount = r.count(r.u16(tl + 0x1A));
    const int subCount = r.count(r.u16(tl + 0x1C));
    const int cutCount = r.count(r.u16(tl + 0x1E));
    f.timelineName = r.str(tl + 0x20);
    const qint64 actorsAt = r.ptr(tl + 0x28), clipsAt = r.ptr(tl + 0x30), oneshotsAt = r.ptr(tl + 0x38);
    const qint64 subsAt = r.ptr(tl + 0x48), cutsAt = r.ptr(tl + 0x50);
    QJsonArray actorJson, clipJson, oneshotJson, cutJson;
    const std::vector<Actor> actors = readActors(r, actorsAt, actorCount, actorStride(actorCount, actorsAt, clipsAt),
                                                 f.timelineActors, actorJson);
    for (int i = 0; i < clipCount; ++i) {
        const qint64 c = clipsAt + qint64(i) * 0x18;
        const float start = std::bit_cast<float>(r.u32(c)), length = std::bit_cast<float>(r.u32(c + 4));
        const quint16 actor = r.u16(c + 8), action = r.u16(c + 10);
        const Item params = container(r, r.ptr(c + 0x10));
        f.clips.push_back({double(start), double(length), double(start + length), actorAction(actors, actor, action), params.text});
        clipJson.append(QJsonObject{{"start", double(start)},
                                    {"duration", double(length)},
                                    {"actor", actor},
                                    {"action", action},
                                    {"params", params.json}});
    }
    for (int i = 0; i < oneshotCount; ++i) {
        const qint64 o = oneshotsAt + qint64(i) * 0x18;
        const float time = std::bit_cast<float>(r.u32(o));
        const quint16 actor = r.u16(o + 4), action = r.u16(o + 6);
        const Item params = container(r, r.ptr(o + 0x10));
        f.oneshots.push_back({double(time), actorAction(actors, actor, action), params.text});
        oneshotJson.append(QJsonObject{{"time", double(time)}, {"actor", actor}, {"action", action}, {"params", params.json}});
    }
    for (int i = 0; i < cutCount; ++i) {
        const qint64 c = cutsAt + qint64(i) * 0x18;
        const float time = std::bit_cast<float>(r.u32(c));
        const QString name = r.str(c + 8);
        const Item params = container(r, r.ptr(c + 0x10));
        f.cuts.push_back({double(time), name, params.text});
        cutJson.append(QJsonObject{{"time", double(time)}, {"name", name}, {"params", params.json}});
    }
    for (int i = 0; i < subCount; ++i)
        f.subtimelines << r.str(subsAt + qint64(i) * 8);
    f.json.insert("timeline", QJsonObject{{"name", f.timelineName},
                                          {"duration", double(f.duration)},
                                          {"actors", actorJson},
                                          {"clips", clipJson},
                                          {"oneshots", oneshotJson},
                                          {"cuts", cutJson},
                                          {"subtimelines", QJsonArray::fromStringList(f.subtimelines)},
                                          {"params", container(r, r.ptr(tl + 0x58)).json}});
}

void readFlowchart(const Reader &r, qint64 chart, Flow &f)
{
    if (r.u32(chart) != 0x4C465645u)
        throw Failure(QObject::tr("The flowchart is missing.").toStdString());
    f.hasChart = true;
    const int actorCount = r.count(r.u16(chart + 0x10));
    const int eventCount = r.count(r.u16(chart + 0x16));
    const int entryCount = r.count(r.u16(chart + 0x18));
    f.name = r.str(chart + 0x20);
    const qint64 actorsAt = r.ptr(chart + 0x28);
    const qint64 eventsAt = r.ptr(chart + 0x30);
    const QStringList entryNames = r.dic(r.ptr(chart + 0x38));
    const qint64 entriesAt = r.ptr(chart + 0x40);
    const qint64 eventSize = f.version >= 0x400 ? 0x30 : 0x28;
    QJsonArray actorJson;
    const std::vector<Actor> actors = readActors(r, actorsAt, actorCount, actorStride(actorCount, actorsAt, eventsAt),
                                                 f.actors, actorJson);

    std::vector<QString> eventNames;
    for (int i = 0; i < eventCount; ++i)
        eventNames.push_back(r.str(eventsAt + qint64(i) * eventSize));
    auto eventRef = [&](quint16 index) {
        if (index == kNone)
            return QObject::tr("end");
        return index < eventNames.size() ? QStringLiteral("#%1 %2").arg(index).arg(eventNames[index])
                                         : QStringLiteral("#%1").arg(index);
    };
    auto cases = [&](qint64 at, int n, QJsonArray *json) {
        QStringList out;
        for (int k = 0; k < n; ++k) {
            const qint64 c = at + qint64(k) * 8;
            const quint32 value = r.u32(c);
            const quint16 target = r.u16(c + 4);
            out << QStringLiteral("%1 → %2").arg(value).arg(eventRef(target));
            json->append(QJsonObject{{"value", qint64(value)}, {"event", target}});
        }
        return out.join(QStringLiteral("; "));
    };
    auto actorName = [&](quint16 index) { return index < actors.size() ? actors[index].name : QStringLiteral("#%1").arg(index); };

    QJsonArray eventJson;
    for (int i = 0; i < eventCount; ++i) {
        const qint64 e = eventsAt + qint64(i) * eventSize;
        const int type = r.u8(e + 8);
        QString kind, actor, action, next;
        Item params;
        QJsonObject json{{"index", i}, {"name", eventNames[size_t(i)]}};
        switch (type) {
        case 0: {
            kind = QObject::tr("action");
            const quint16 a = r.u16(e + 0x0C), act = r.u16(e + 0x0E);
            actor = actorName(a);
            action = a < actors.size() && act < actors[a].actions.size() ? actors[a].actions[act] : QStringLiteral("#%1").arg(act);
            params = container(r, r.ptr(e + 0x10));
            next = eventRef(r.u16(e + 0x0A));
            json.insert("next", r.u16(e + 0x0A));
            break;
        }
        case 1: {
            kind = QObject::tr("switch");
            const int n = r.count(r.u16(e + 0x0A));
            const quint16 a = r.u16(e + 0x0C), q = r.u16(e + 0x0E);
            actor = actorName(a);
            action = a < actors.size() && q < actors[a].queries.size() ? actors[a].queries[q] : QStringLiteral("#%1").arg(q);
            params = container(r, r.ptr(e + 0x10));
            QJsonArray caseJson;
            next = cases(r.ptr(e + 0x18), n, &caseJson);
            json.insert("cases", caseJson);
            break;
        }
        case 2: {
            kind = QObject::tr("fork");
            const int n = r.count(r.u16(e + 0x0A));
            const qint64 forks = r.ptr(e + 0x10);
            QStringList branches;
            QJsonArray forkJson;
            for (int k = 0; k < n && forks; ++k) {
                branches << eventRef(r.u16(forks + qint64(k) * 2));
                forkJson.append(r.u16(forks + qint64(k) * 2));
            }
            next = QObject::tr("%1; join %2").arg(branches.join(QStringLiteral(", ")), eventRef(r.u16(e + 0x0C)));
            json.insert("forks", forkJson);
            json.insert("join", r.u16(e + 0x0C));
            break;
        }
        case 3:
            kind = QObject::tr("join");
            next = eventRef(r.u16(e + 0x0A));
            json.insert("next", r.u16(e + 0x0A));
            break;
        case 4: {
            kind = QObject::tr("sub flow");
            params = container(r, r.ptr(e + 0x10));
            const QString flow = r.str(e + 0x18), entry = r.str(e + 0x20);
            action = (flow.isEmpty() ? QString() : flow + u'<') + entry + (flow.isEmpty() ? QString() : QStringLiteral(">"));
            next = eventRef(r.u16(e + 0x0A));
            json.insert("flowchart", flow);
            json.insert("entry_point", entry);
            json.insert("next", r.u16(e + 0x0A));
            break;
        }
        case 5: {
            kind = QObject::tr("sub flow switch");
            const int n = r.count(r.u16(e + 0x0A));
            params = container(r, r.ptr(e + 0x10));
            QJsonArray caseJson;
            next = cases(r.ptr(e + 0x18), n, &caseJson);
            const QString flow = r.str(e + 0x20), entry = r.str(e + 0x28);
            action = (flow.isEmpty() ? QString() : flow + u'<') + entry + (flow.isEmpty() ? QString() : QStringLiteral(">"));
            json.insert("flowchart", flow);
            json.insert("entry_point", entry);
            json.insert("cases", caseJson);
            break;
        }
        case 6:
            kind = QObject::tr("return");
            next = QObject::tr("return %1").arg(r.u32(e + 0x0C));
            json.insert("value", qint64(r.u32(e + 0x0C)));
            break;
        default:
            kind = QObject::tr("type %1").arg(type);
            break;
        }
        json.insert("kind", kind);
        if (!actor.isEmpty())
            json.insert("actor", actor);
        if (!action.isEmpty())
            json.insert(type == 1 ? "query" : type == 0 ? "action" : "target", action);
        if (!params.text.isEmpty())
            json.insert("params", params.json);
        f.events.push_back({i, eventNames[size_t(i)], kind, actor, action, params.text, next});
        eventJson.append(json);
    }

    QJsonArray entryJson;
    for (int i = 0; i < entryCount && i < entryNames.size(); ++i) {
        const qint64 e = entriesAt + qint64(i) * 0x20;
        const QStringList variables = r.dic(r.ptr(e + 8));
        const quint16 first = r.u16(e + 0x1C);
        f.entries.push_back({entryNames[i], eventRef(first), variables.join(QStringLiteral(", "))});
        entryJson.append(QJsonObject{{"name", entryNames[i]}, {"event", first}, {"variables", QJsonArray::fromStringList(variables)}});
    }
    f.json.insert("entry_points", entryJson);
    f.json.insert("actors", actorJson);
    f.json.insert("events", eventJson);
}

Flow parseFlow(const QByteArray &data)
{
    if (data.size() < 0x48 || !data.startsWith(QByteArray("BFEVFL\0\0", 8)))
        throw Failure(QObject::tr("Not an event flow (no \"BFEVFL\" at the start).").toStdString());
    const Reader r(data);
    Flow f;
    f.version = r.u16(8);
    if (r.u16(0x0C) != 0xFEFF)
        throw Failure(QObject::tr("Only little-endian event flows are supported.").toStdString());
    f.json.insert("version", QStringLiteral("0x%1").arg(f.version, 0, 16));
    if (r.u16(0x20) > 0)
        readFlowchart(r, r.ptr(r.ptr(0x28)), f);
    if (r.u16(0x22) > 0)
        readTimeline(r, r.ptr(r.ptr(0x38)), f);
    if (!f.hasChart && !f.hasTimeline)
        throw Failure(QObject::tr("This event file has neither a flowchart nor a timeline.").toStdString());
    f.json.insert("name", f.hasChart ? f.name : f.timelineName);
    return f;
}

}

int EvflReader::score(const QString &, QByteArrayView head) const
{
    return head.startsWith(QByteArrayView("BFEVFL\0\0", 8)) ? 100 : 0;
}

std::unique_ptr<Document> EvflReader::read(const QString &path, QString *error) const
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

std::unique_ptr<Document> EvflReader::readBytes(const QByteArray &data, const QString &, QString *error) const
{
    Flow f;
    try {
        f = parseFlow(data);
    } catch (const Failure &failure) {
        if (error)
            *error = QString::fromStdString(failure.what());
        return nullptr;
    }
    const QJsonObject json = f.json;
    const auto exporter = [json] { return QJsonDocument(json).toJson(); };
    auto doc = std::make_unique<Document>();
    auto add = [&](const QString &name, std::vector<SimpleColumn> columns, std::vector<SimpleTable::Row> rows,
                   const QString &description) {
        auto table = std::make_shared<SimpleTable>(name, std::move(columns), std::move(rows));
        table->setDescription(description);
        table->setJsonExporter(exporter);
        doc->tables.push_back(std::move(table));
    };
    const std::vector<SimpleColumn> actorColumns{{QObject::tr("actor")}, {QObject::tr("argument")}, {QObject::tr("actions")},
                                                 {QObject::tr("queries")}, {QObject::tr("parameters")}};
    if (f.hasChart) {
        const int eventCount = int(f.events.size());
        add(QObject::tr("Events"),
            {{QObject::tr("index")}, {QObject::tr("event")}, {QObject::tr("kind")}, {QObject::tr("actor")},
             {QObject::tr("action / query")}, {QObject::tr("parameters")}, {QObject::tr("next")}},
            std::move(f.events),
            QObject::tr("%1, version 0x%2: %n event(s)", nullptr, eventCount).arg(f.name).arg(f.version, 0, 16));
        add(QObject::tr("Entry points"),
            {{QObject::tr("entry point")}, {QObject::tr("first event")}, {QObject::tr("variables")}}, std::move(f.entries),
            QObject::tr("%1: where the event flow can be started").arg(f.name));
        add(QObject::tr("Actors"), actorColumns, std::move(f.actors), {});
    }
    if (f.hasTimeline) {
        const QString summary = QObject::tr("%1: %2 frames, %n clip(s)", nullptr, int(f.clips.size()))
                                    .arg(f.timelineName)
                                    .arg(double(f.duration));
        add(QObject::tr("Timeline clips"),
            {{QObject::tr("start")}, {QObject::tr("duration")}, {QObject::tr("end")}, {QObject::tr("actor · action")},
             {QObject::tr("parameters")}},
            std::move(f.clips),
            f.subtimelines.isEmpty() ? summary
                                     : summary + QObject::tr("; sub-timelines: %1").arg(f.subtimelines.join(QStringLiteral(", "))));
        if (!f.oneshots.empty())
            add(QObject::tr("Timeline one-shots"),
                {{QObject::tr("time")}, {QObject::tr("actor · action")}, {QObject::tr("parameters")}}, std::move(f.oneshots), {});
        if (!f.cuts.empty())
            add(QObject::tr("Timeline cuts"), {{QObject::tr("time")}, {QObject::tr("cut")}, {QObject::tr("parameters")}},
                std::move(f.cuts), {});
        add(QObject::tr("Timeline actors"), actorColumns, std::move(f.timelineActors), {});
    }
    return doc;
}
