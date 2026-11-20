#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <optional>
#include <vector>

struct SarcEntry
{
    QString name;
    QByteArray data;
    qsizetype alignment = 0;
    quint32 hash = 0;
};

struct SarcLayout
{
    bool littleEndian = true;
    quint32 hashMultiplier = 0x65;
    quint16 version = 0x0100;
};

struct SarcContents
{
    SarcLayout layout;
    std::vector<SarcEntry> entries;
};

struct SarcNode
{
    quint32 hash = 0;
    quint32 attributes = 0;
    qsizetype start = 0;
    qsizetype end = 0;
    QString name;
    bool inside = true;
    bool badName = false;
};

struct SarcIndex
{
    SarcLayout layout;
    qsizetype nodesOffset = 0;
    qsizetype dataOffset = 0;
    std::vector<SarcNode> nodes;
};

constexpr qsizetype kSarcDefaultAlignment = 4;
constexpr qsizetype kSarcMaxInferredAlignment = 0x2000;

std::optional<SarcIndex> parseSarcIndex(const QByteArray &data, QString *error);
QString sarcHashName(quint32 hash);
QString sarcNodeName(const SarcNode &node);
quint32 sarcNameHash(QByteArrayView name, quint32 multiplier);
QString sarcEntryName(const SarcEntry &entry);
std::optional<QByteArray> writeSarc(const std::vector<SarcEntry> &entries, const SarcLayout &layout, QString *error);
std::optional<SarcContents> readSarc(const QByteArray &data, QString *error);
std::optional<QByteArray> replaceSarcEntry(const QByteArray &sarc, const QString &name, const QByteArray &data,
                                           QString *error);
