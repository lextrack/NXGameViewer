#include "ui/mainwindow.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace {

constexpr QLatin1StringView kAppName("NX Game Viewer");
constexpr QLatin1StringView kLegacyOrganization("TestCpp");
constexpr QLatin1StringView kLegacyApplication("GameFileViewer");

void migrateLegacySettings()
{
    QSettings current;
    if (!current.allKeys().isEmpty())
        return;
    const QSettings legacy(kLegacyOrganization, kLegacyApplication);
    for (const QString &key : legacy.allKeys())
        current.setValue(key, legacy.value(key));
}

void migrateLegacyData()
{
    const QString current = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (current.isEmpty() || QFileInfo::exists(current))
        return;
    QApplication::setOrganizationName(kLegacyOrganization);
    QApplication::setApplicationName(kLegacyApplication);
    const QString legacy = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QApplication::setOrganizationName(kAppName);
    QApplication::setApplicationName(kAppName);
    if (legacy.isEmpty() || !QFileInfo(legacy).isDir())
        return;
    QDir().mkpath(QFileInfo(current).absolutePath());
    QDir().rename(legacy, current);
}

}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setAttribute(Qt::AA_DontShowIconsInMenus);
    QApplication::setOrganizationName(kAppName);
    QApplication::setApplicationName(kAppName);
    migrateLegacySettings();
    migrateLegacyData();

    const QStringList args = QApplication::arguments();
    const QString startPath = args.size() > 1 ? args.at(1) : QString();

    MainWindow window(startPath);
    window.show();
    return app.exec();
}
