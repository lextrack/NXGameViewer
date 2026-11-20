#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringList>

#include <optional>

struct SfntInfo
{
    QString family;
    QString style;
    QString fullName;
    QString version;
    QString postScriptName;
    QString outlines;
    int glyphs = 0;
    int fonts = 1;
    QStringList tables;
};

bool isSfnt(QByteArrayView head);
bool isNintendoFont(QByteArrayView head);
std::optional<QByteArray> decryptNintendoFont(QByteArrayView data);
std::optional<SfntInfo> sfntInfo(const QByteArray &font);
QString sfntSuffix(const QByteArray &font);
