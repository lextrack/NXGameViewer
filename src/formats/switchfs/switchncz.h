#pragma once

#include "switchfs/switchnca.h"

#include <QString>

constexpr qint64 kNczHeaderSize = 0x4000;

StoragePtr nczStorage(const StoragePtr &file, QString *error);
StoragePtr zstdFrameStorage(const StoragePtr &packed, qint64 size);
