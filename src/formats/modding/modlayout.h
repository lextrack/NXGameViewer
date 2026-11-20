#pragma once

#include <QString>
#include <QStringView>

#include <optional>

enum class ModLayout { YuzuFamily, Ryujinx, Atmosphere };

struct ModLocation
{
    ModLayout layout = ModLayout::YuzuFamily;
    QString base;
    QString titleId;
    QString modName;
};

bool layoutHasModName(ModLayout layout);
bool isTitleId(QStringView text);
std::optional<QString> titleIdFromPath(const QString &path);
QString applicationTitleId(const QString &titleId);
bool sameGame(const QString &a, const QString &b);
std::optional<QString> titleIdFolder(ModLayout layout, const QString &base);
QString suggestedEmulatorFolder(const QString &path);
QString modNameProblem(const QString &name);
QString modRomfsPath(const ModLocation &location);
std::optional<ModLocation> parseModFolder(const QString &romfs);
QString matchExistingCase(const QString &parent, const QString &name);
