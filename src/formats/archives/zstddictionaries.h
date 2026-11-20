#pragma once

#include "core/format.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

struct ZSTD_DDict_s;

int addZstdDictionaries(const QByteArray &zsDicPack, const QString &source);
bool loadZstdDictionariesNear(const QString &path);
bool loadZstdDictionariesFrom(const Archive &archive);
unsigned zstdDictionaryId(QByteArrayView data);
const ZSTD_DDict_s *zstdDictionary(unsigned id);
QByteArray zstdDictionaryBytes(unsigned id);
bool needsUnknownZstdDictionary(QByteArrayView data);
QStringList zstdDictionaryRoots();
