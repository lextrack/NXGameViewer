#include "core/paths.h"

#include <QStringList>

namespace {

bool isDriveLetter(const QString &part)
{
    return part.size() >= 2 && part[1] == u':' && part[0].isLetter();
}

}

QString safeFileName(QString name)
{
    static const QString forbidden = QStringLiteral("/\\:*?\"<>|");
    for (QChar &c : name) {
        if (forbidden.contains(c) || c.unicode() < 0x20)
            c = u'_';
    }
    return name.trimmed();
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
        const QString safe = safeFileName(part);
        if (!safe.isEmpty() && safe != QLatin1String(".") && safe != QLatin1String(".."))
            parts << safe;
    }
    return parts.join(u'/');
}
