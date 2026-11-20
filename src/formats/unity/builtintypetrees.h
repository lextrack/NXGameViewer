#pragma once

#include "unity/serializedfile.h"

#include <vector>

std::vector<TypeTreeNode> builtinTypeTree(qint32 classId, const QString &unityVersion);
