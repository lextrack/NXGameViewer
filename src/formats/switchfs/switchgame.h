#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <vector>

class SwitchKeys;

struct SwitchTitle
{
    enum Type { Application, Patch, AddOn, Other };

    QString package;
    quint64 titleId = 0;
    Type type = Other;
    quint32 version = 0;
    QString name;
    QString publisher;
    QString displayVersion;
    QByteArray icon;
    QString problem;
    QStringList programs;
    QStringList data;
    int nupPackage = -1;

    QString titleIdText() const { return QStringLiteral("%1").arg(titleId, 16, 16, QLatin1Char('0')).toUpper(); }
    QString typeName() const;
    QString displayName() const;
    QString folderName() const;
    bool canExtract() const { return problem.isEmpty(); }
};

struct SwitchExtractResult
{
    int files = 0;
    qint64 bytes = 0;
    bool canceled = false;
};

struct SwitchTitleFile
{
    QString path;
    qint64 size = 0;
    std::function<bool(qint64 offset, char *out, qint64 length)> read;
    bool seeksFast = true;
    bool sequential = false;
};

struct SwitchKeyCheck
{
    bool opened = false;
    bool readsHeaders = false;
    bool hasGenerationKeys = false;
    int neededGeneration = -1;

    bool works() const { return readsHeaders && hasGenerationKeys; }
};

SwitchKeyCheck checkSwitchPackageKeys(const QString &package, const SwitchKeys &keys);
std::vector<SwitchTitle> scanSwitchPackage(const QString &path, const SwitchKeys &keys, QString *error);
std::optional<std::vector<SwitchTitleFile>> switchTitleFiles(const SwitchTitle &title, const SwitchKeys &keys,
                                                             QString *error);
bool verifySwitchTitle(const SwitchTitle &title, const SwitchKeys &keys,
                       const std::function<bool(qint64 done, qint64 total)> &progress, QString *error);
bool extractSwitchTitle(const SwitchTitle &title, const SwitchKeys &keys, const QString &destination,
                        const std::function<bool(qint64 done, qint64 total)> &progress, SwitchExtractResult *result,
                        QString *error);
bool extractSwitchFiles(const std::vector<SwitchTitleFile> &files, const QString &destination,
                        const std::function<bool(qint64 done, qint64 total)> &progress, SwitchExtractResult *result,
                        QString *error);
