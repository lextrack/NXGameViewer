#pragma once

#include "switchfs/switchgame.h"
#include "switchfs/switchkeys.h"

#include <QString>
#include <QStringList>

#include <optional>

struct KeySearchPlaces
{
    QStringList gameFolders;
    QString home;
    QString appData;
    QString localAppData;
    QString dataHome;
    QString configHome;

    static KeySearchPlaces forSystem(const QStringList &packages);
    static QStringList foldersOf(const QStringList &packages);
};

struct FoundSwitchKeys
{
    QString prodKeys;
    QStringList titleKeyFiles;
    SwitchKeys keys;
    int newestGeneration = -1;
    int neededGeneration = -1;
    bool testedOnGame = false;
    bool readsGame = false;
    bool usable = true;
};

struct SwitchKeySearch
{
    std::optional<FoundSwitchKeys> found;
    QString problem;
};

const QStringList &yuzuFamilyNames();
const QStringList &ryujinxFamilyNames();
QStringList emulatorNameSpellings(const QString &name);
QStringList switchKeyFolders(const KeySearchPlaces &places);
SwitchKeySearch findSwitchKeys(const QStringList &packages, const KeySearchPlaces &places);
SwitchKeySearch useSwitchKeyFile(const QString &prodKeys, const QStringList &packages, const KeySearchPlaces &places);
