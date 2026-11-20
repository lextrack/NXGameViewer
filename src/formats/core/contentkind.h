#pragma once

#include <QByteArrayView>
#include <QString>

enum class ContentCategory { Archive, Compressed, Model, Texture, Image, Sound, Text, Data, Other };

QString describeContent(QByteArrayView head);
ContentCategory contentCategory(QByteArrayView head);
