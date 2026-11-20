#include "modding/modlayout.h"

#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QStringList>

namespace {

QStringList segments(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).split(u'/');
}

bool same(const QString &a, const QString &b)
{
    return a.compare(b, Qt::CaseInsensitive) == 0;
}

QString join(const QString &base, const QStringList &parts)
{
    QString out = QDir::cleanPath(QDir::fromNativeSeparators(base));
    for (const QString &part : parts) {
        if (!out.endsWith(u'/'))
            out += u'/';
        out += part;
    }
    return out;
}

bool endsWith(const QString &base, const QStringList &tail)
{
    const QStringList parts = segments(base);
    if (parts.size() < tail.size())
        return false;
    for (qsizetype i = 0; i < tail.size(); ++i)
        if (!same(parts[parts.size() - tail.size() + i], tail[i]))
            return false;
    return true;
}

QStringList titleParent(ModLayout layout)
{
    switch (layout) {
    case ModLayout::YuzuFamily: return {QStringLiteral("load")};
    case ModLayout::Ryujinx: return {QStringLiteral("mods"), QStringLiteral("contents")};
    case ModLayout::Atmosphere: return {QStringLiteral("atmosphere"), QStringLiteral("contents")};
    }
    return {};
}

QString titleContainer(ModLayout layout, QString base)
{
    if (titleIdFolder(layout, base))
        base = QFileInfo(QDir::cleanPath(QDir::fromNativeSeparators(base))).path();
    const QString load = QStringLiteral("load"), mods = QStringLiteral("mods"),
                  contents = QStringLiteral("contents"), atmosphere = QStringLiteral("atmosphere");
    switch (layout) {
    case ModLayout::YuzuFamily:
        return endsWith(base, {load}) ? join(base, {}) : join(base, {load});
    case ModLayout::Ryujinx:
        if (endsWith(base, {mods, contents}))
            return join(base, {});
        return endsWith(base, {mods}) ? join(base, {contents}) : join(base, {mods, contents});
    case ModLayout::Atmosphere:
        if (endsWith(base, {atmosphere, contents}))
            return join(base, {});
        return endsWith(base, {atmosphere}) ? join(base, {contents}) : join(base, {atmosphere, contents});
    }
    return {};
}

}

bool layoutHasModName(ModLayout layout)
{
    return layout != ModLayout::Atmosphere;
}

bool isTitleId(QStringView text)
{
    if (text.size() != 16)
        return false;
    for (QChar c : text)
        if (!((c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f') || (c >= u'A' && c <= u'F')))
            return false;
    return true;
}

std::optional<QString> titleIdFromPath(const QString &path)
{
    const QStringList parts = segments(path);
    std::optional<QString> fallback;
    for (qsizetype i = parts.size() - 1; i >= 0; --i) {
        if (!isTitleId(parts[i]))
            continue;
        if (i + 1 < parts.size() && (same(parts[i + 1], QStringLiteral("romfs")) || same(parts[i + 1], QStringLiteral("exefs"))))
            return parts[i].toUpper();
        if (!fallback)
            fallback = parts[i].toUpper();
    }
    return fallback;
}

QString applicationTitleId(const QString &titleId)
{
    if (!isTitleId(titleId) || !titleId.endsWith(QLatin1String("800")))
        return titleId.toUpper();
    return titleId.left(13).toUpper() + QStringLiteral("000");
}

bool sameGame(const QString &a, const QString &b)
{
    if (!isTitleId(a) || !isTitleId(b))
        return false;
    const quint64 mask = ~quint64(0x1FFF);
    return (a.toULongLong(nullptr, 16) & mask) == (b.toULongLong(nullptr, 16) & mask);
}

std::optional<QString> titleIdFolder(ModLayout layout, const QString &base)
{
    const QStringList parts = segments(base);
    const QStringList parent = titleParent(layout);
    if (parts.size() < parent.size() + 1 || !isTitleId(parts.last()))
        return std::nullopt;
    if (!endsWith(parts.mid(0, parts.size() - 1).join(u'/'), parent))
        return std::nullopt;
    return parts.last().toUpper();
}

QString suggestedEmulatorFolder(const QString &path)
{
    const QStringList parts = segments(path);
    for (qsizetype i = parts.size() - 1; i >= 2; --i)
        if (isTitleId(parts[i]) && same(parts[i - 1], QStringLiteral("dump")))
            return parts.mid(0, i - 1).join(u'/');
    return {};
}

QString modNameProblem(const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return QObject::tr("Type a name for the mod.");
    if (trimmed == QLatin1String(".") || trimmed == QLatin1String(".."))
        return QObject::tr("\"%1\" cannot be used as a folder name.").arg(trimmed);
    static const QString forbidden = QStringLiteral("/\\:*?\"<>|");
    for (QChar c : trimmed) {
        if (c.unicode() < 0x20)
            return QObject::tr("The name cannot contain control characters.");
        if (forbidden.contains(c))
            return QObject::tr("The name cannot contain %1").arg(c);
    }
    if (trimmed.endsWith(u'.'))
        return QObject::tr("The name cannot end with a dot.");
    return {};
}

QString modRomfsPath(const ModLocation &location)
{
    const QString container = titleContainer(location.layout, location.base);
    const QString id = matchExistingCase(container, location.titleId.toUpper());
    if (!layoutHasModName(location.layout))
        return join(container, {id, QStringLiteral("romfs")});
    const QString name = location.modName.trimmed();
    return join(container, {id, matchExistingCase(join(container, {id}), name), QStringLiteral("romfs")});
}

std::optional<ModLocation> parseModFolder(const QString &romfs)
{
    const QStringList p = segments(romfs);
    const qsizetype n = p.size();
    if (n < 3 || !same(p[n - 1], QStringLiteral("romfs")))
        return std::nullopt;
    if (n >= 4 && isTitleId(p[n - 2]) && same(p[n - 3], QStringLiteral("contents"))
        && same(p[n - 4], QStringLiteral("atmosphere")))
        return ModLocation{ModLayout::Atmosphere, p.mid(0, n - 4).join(u'/'), p[n - 2].toUpper(), {}};
    if (n < 5 || !isTitleId(p[n - 3]))
        return std::nullopt;
    const QString id = p[n - 3].toUpper();
    const QString name = p[n - 2];
    if (same(p[n - 4], QStringLiteral("load")))
        return ModLocation{ModLayout::YuzuFamily, p.mid(0, n - 4).join(u'/'), id, name};
    if (n >= 6 && same(p[n - 4], QStringLiteral("contents")) && same(p[n - 5], QStringLiteral("mods")))
        return ModLocation{ModLayout::Ryujinx, p.mid(0, n - 5).join(u'/'), id, name};
    return std::nullopt;
}

QString matchExistingCase(const QString &parent, const QString &name)
{
    const QStringList existing = QDir(parent).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    if (existing.contains(name))
        return name;
    for (const QString &entry : existing)
        if (entry.compare(name, Qt::CaseInsensitive) == 0)
            return entry;
    return name;
}
