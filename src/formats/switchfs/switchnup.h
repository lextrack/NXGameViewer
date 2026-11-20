#pragma once

#include "switchfs/switchgame.h"

#include <QString>

#include <functional>
#include <optional>
#include <vector>

bool isNupFile(const QString &path);
std::vector<SwitchTitle> scanNupPackage(const QString &path, QString *error);
std::optional<std::vector<SwitchTitleFile>> nupTitleFiles(const SwitchTitle &title, QString *error);
bool verifyNupTitle(const SwitchTitle &title, const std::function<bool(qint64 done, qint64 total)> &progress,
                    QString *error);
