#pragma once

#include "modding/modlayout.h"

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>
#include <optional>
#include <vector>

struct KeySearchPlaces;

struct ModRecord
{
    QString id;
    QString name;
    QString titleId;
    QString game;
    ModLayout layout = ModLayout::YuzuFamily;
    QString folder;
    QString lastSavedFile;
    QDateTime lastSaved;

    bool exists() const;
    QString emulatorName() const;
};

struct ModFile
{
    QString path;
    qint64 size = 0;
    QDateTime modified;
};

constexpr int kMaxModHistory = 200;

QString modId(const QString &romfs);
ModRecord modRecord(const QString &romfs, const QString &game = {});
QString modDisplayName(const ModRecord &record);
QVariantMap modRecordToVariant(const ModRecord &record);
std::optional<ModRecord> modRecordFromVariant(const QVariantMap &map);
void rememberMod(std::vector<ModRecord> &history, ModRecord record);
void forgetMod(std::vector<ModRecord> &history, const QString &id);
void forgetMissingMods(std::vector<ModRecord> &history);

QString gameNameFromPath(const QString &path, const QString &titleId);

QStringList emulatorDataFolders(const KeySearchPlaces &places);
std::vector<ModRecord> findInstalledMods(const QStringList &dataFolders);

QString modRoot(const QString &romfs);
std::vector<ModFile> listModFiles(const QString &romfs);
bool removeModFile(const QString &romfs, const QString &relative, bool toTrash, QString *error);
QString modDeleteTarget(const QString &romfs);
bool deleteMod(const QString &romfs, bool toTrash, QString *error);

QString zipPathFor(const ModRecord &record, const QString &relative);
bool packModZip(const ModRecord &record, const QString &zipPath, const std::function<bool(int done, int total)> &progress,
                QString *error);
