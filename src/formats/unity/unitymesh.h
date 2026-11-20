#pragma once

#include "models/model3d.h"
#include "unity/serializedfile.h"

#include <QByteArray>
#include <QString>

#include <memory>

std::shared_ptr<Model3D> unityMeshModel(const UValue &mesh, const QByteArray &streamed, const QString &unityVersion,
                                        bool bigEndian, QString *error, std::vector<int> *submeshOfPrimitive = nullptr);
