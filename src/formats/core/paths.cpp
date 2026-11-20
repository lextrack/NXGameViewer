#include "core/paths.h"

#include <QStringList>

#include <algorithm>

namespace {

bool isDriveLetter(const QString &part)
{
    return part.size() >= 2 && part[1] == u':' && part[0].isLetter();
}

}

Qt::CaseSensitivity pathCaseSensitivity()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

QString safeFileName(QString name)
{
    static const QString forbidden = QStringLiteral("/\\:*?\"<>|");
    for (QChar &c : name) {
        if (forbidden.contains(c) || c.unicode() < 0x20)
            c = u'_';
    }
    name = name.trimmed();
    if (!name.isEmpty() && std::all_of(name.cbegin(), name.cend(), [](QChar c) { return c == u'.'; }))
        name.fill(u'_');
    return name;
}

bool isSafeRelativePath(const QString &path)
{
    if (path.isEmpty() || path.startsWith(u'/') || path.contains(u'\\') || path.contains(QChar(u'\0')))
        return false;
    for (const QString &part : path.split(u'/'))
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..") || isDriveLetter(part))
            return false;
    return true;
}

QString sanitizedRelativePath(const QString &path)
{
    QString unified = path;
    unified.replace(u'\\', u'/');
    QStringList parts;
    for (const QString &part : unified.split(u'/', Qt::SkipEmptyParts)) {
        if (part == QLatin1String(".") || part == QLatin1String(".."))
            continue;
        if (const QString safe = safeFileName(part); !safe.isEmpty())
            parts << safe;
    }
    return parts.join(u'/');
}
