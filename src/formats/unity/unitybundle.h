#pragma once

#include <QByteArray>
#include <QString>

#include "core/storedreader.h"

#include <functional>
#include <optional>
#include <vector>

struct UnityBundleNode
{
    QString path;
    qint64 offset = 0;
    qint64 size = 0;
    quint32 flags = 0;
};

struct UnityBundleBlock
{
    qint64 offset = 0;
    qint64 size = 0;
    qint64 storedOffset = 0;
    qint64 storedSize = 0;
    quint16 flags = 0;
};

struct UnityBundle
{
    using StoredReader = ::StoredReader;

    quint32 formatVersion = 0;
    QString unityVersion;
    QString unityRevision;
    std::vector<UnityBundleNode> nodes;
    std::vector<UnityBundleBlock> blocks;
    StoredReader stored;

    std::optional<QByteArray> read(size_t node, qint64 offset, qint64 size, QString *error = nullptr) const;
    std::optional<QByteArray> nodeData(size_t index, QString *error = nullptr) const;
    int find(const QString &path) const;
};

std::optional<UnityBundle> readUnityBundle(const QByteArray &file, QString *error);
std::optional<UnityBundle> readUnityBundleFile(const QString &path, QString *error);
std::optional<UnityBundle> readUnityBundleStored(const StoredReader &read, qint64 size, QString *error);
