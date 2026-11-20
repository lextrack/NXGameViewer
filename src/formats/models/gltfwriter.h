#pragma once

#include "core/format.h"
#include "models/model3d.h"

struct GltfOptions
{
    bool includeAuxiliaryMeshes = false;
    bool flipV = true;
    bool bakeMirroredRepeat = true;
};

QByteArray encodePng(const Image &image);
QByteArray writeGlb(const Model3D &model, const std::vector<std::shared_ptr<const Image>> &textures,
                    const GltfOptions &options = {}, const std::vector<ModelAnimation> &animations = {});
