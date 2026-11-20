#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

struct TypeTreeNode
{
    QString type;
    QString name;
    int level = 0;
    qint32 byteSize = 0;
    quint32 metaFlags = 0;
    std::vector<int> children;

    bool aligned() const { return metaFlags & 0x4000; }
};

struct SerializedType
{
    qint32 classId = 0;
    std::vector<TypeTreeNode> nodes;
    bool builtin = false;

    QString className() const;
};

struct SerializedObject
{
    qint64 pathId = 0;
    qint64 offset = 0;
    qint64 size = 0;
    int typeIndex = -1;
};

struct SerializedFile
{
    quint32 version = 0;
    bool bigEndian = false;
    QString unityVersion;
    qint32 platform = 0;
    bool hasTypeTrees = false;
    std::vector<SerializedType> types;
    std::vector<SerializedObject> objects;
    QStringList externals;
    QByteArray data;

    const SerializedType *typeOf(const SerializedObject &object) const;
    QByteArray objectData(const SerializedObject &object) const { return data.mid(object.offset, object.size); }
    const SerializedObject *find(qint64 pathId) const;
};

QString unityClassName(qint32 classId);
bool looksLikeSerializedFile(QByteArrayView head);
std::optional<SerializedFile> readSerializedFile(const QByteArray &data, QString *error);

struct UValue
{
    enum class Kind { Null, Bool, Int, UInt, Float, String, Bytes, Array, Struct };

    Kind kind = Kind::Null;
    QString name;
    QString type;
    qint64 i = 0;
    quint64 u = 0;
    double f = 0;
    QByteArray bytes;
    QString elementType;
    int elementSize = 0;
    std::vector<UValue> children;

    const UValue *child(QStringView field) const;
    const UValue *at(QStringView path) const;
    qint64 toInt() const;
    double toDouble() const;
    QString toString() const;
    qint64 count() const;
};

std::optional<UValue> readTypeTreeValue(const std::vector<TypeTreeNode> &nodes, QByteArrayView data, bool bigEndian,
                                        QString *error, qsizetype *consumed = nullptr);
QString objectName(const UValue &value);
QString readObjectName(const std::vector<TypeTreeNode> &nodes, QByteArrayView data, bool bigEndian);
