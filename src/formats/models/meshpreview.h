#pragma once

#include "core/format.h"

#include <array>
#include <vector>

struct PreviewMesh
{
    std::vector<std::array<float, 3>> positions;
    std::vector<quint32> indices;
};

constexpr qint64 kDefaultPreviewWork = 150'000'000;
Image renderPreview(const std::vector<PreviewMesh> &meshes, int size = 640,
                    qint64 maxWork = kDefaultPreviewWork, bool *complete = nullptr);

std::vector<PreviewMesh> previewMeshes(const Model3D &model);
