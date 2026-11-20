#include "models/meshpreview.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace {

using Vec3 = std::array<float, 3>;

Vec3 sub(const Vec3 &a, const Vec3 &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 cross(const Vec3 &a, const Vec3 &b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
float dot(const Vec3 &a, const Vec3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

constexpr std::array<std::array<int, 3>, 8> kPalette = {{
    {214, 180, 140}, {120, 160, 210}, {200, 120, 120}, {140, 190, 130},
    {190, 150, 210}, {220, 200, 120}, {120, 190, 190}, {200, 160, 180},
}};

}

Image renderPreview(const std::vector<PreviewMesh> &meshes, int size, qint64 maxWork, bool *complete)
{
    if (complete)
        *complete = true;
    qint64 work = 0;
    Image image;
    image.width = image.height = size;
    image.rgba = QByteArray(qsizetype(size) * size * 4, Qt::Uninitialized);
    for (qsizetype i = 0; i < qsizetype(size) * size; ++i) {
        image.rgba[i * 4 + 0] = char(48);
        image.rgba[i * 4 + 1] = char(52);
        image.rgba[i * 4 + 2] = char(58);
        image.rgba[i * 4 + 3] = char(255);
    }

    const float yaw = 30.f * float(std::numbers::pi) / 180.f, pitch = 20.f * float(std::numbers::pi) / 180.f;
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    auto view = [&](const Vec3 &p) -> Vec3 {
        const float x = cy * p[0] + sy * p[2];
        const float z = -sy * p[0] + cy * p[2];
        return {x, cp * p[1] - sp * z, sp * p[1] + cp * z};
    };

    float minX = std::numeric_limits<float>::max(), maxX = -minX, minY = minX, maxY = -minX;
    for (const PreviewMesh &m : meshes) {
        for (quint32 index : m.indices) {
            if (index >= m.positions.size())
                continue;
            const Vec3 v = view(m.positions[index]);
            if (!std::isfinite(v[0]) || !std::isfinite(v[1]))
                continue;
            minX = std::min(minX, v[0]);
            maxX = std::max(maxX, v[0]);
            minY = std::min(minY, v[1]);
            maxY = std::max(maxY, v[1]);
        }
    }
    if (minX > maxX)
        return image;
    const float extent = std::max({maxX - minX, maxY - minY, 1e-6f});
    const float scale = (size * 0.9f) / extent;
    const float offsetX = size / 2.f - (minX + maxX) / 2.f * scale;
    const float offsetY = size / 2.f + (minY + maxY) / 2.f * scale;

    std::vector<float> depth(size_t(size) * size, -std::numeric_limits<float>::max());
    const Vec3 light = {0.35f, 0.6f, 0.72f};
    auto *out = reinterpret_cast<uchar *>(image.rgba.data());

    for (size_t meshIndex = 0; meshIndex < meshes.size(); ++meshIndex) {
        const PreviewMesh &m = meshes[meshIndex];
        const auto &colour = kPalette[meshIndex % kPalette.size()];
        for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
            const quint32 i0 = m.indices[t], i1 = m.indices[t + 1], i2 = m.indices[t + 2];
            if (i0 >= m.positions.size() || i1 >= m.positions.size() || i2 >= m.positions.size())
                continue;
            const Vec3 a = view(m.positions[i0]), b = view(m.positions[i1]), c = view(m.positions[i2]);
            Vec3 n = cross(sub(b, a), sub(c, a));
            const float length = std::sqrt(dot(n, n));
            if (!(length > 0) || !std::isfinite(length))
                continue;
            n = {n[0] / length, n[1] / length, n[2] / length};
            const float shade = 0.25f + 0.75f * std::abs(dot(n, light));

            const float x0 = a[0] * scale + offsetX, y0 = offsetY - a[1] * scale;
            const float x1 = b[0] * scale + offsetX, y1 = offsetY - b[1] * scale;
            const float x2 = c[0] * scale + offsetX, y2 = offsetY - c[1] * scale;
            const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
            if (std::abs(area) < 1e-12f)
                continue;
            const int left = std::max(0, int(std::floor(std::min({x0, x1, x2}))));
            const int right = std::min(size - 1, int(std::ceil(std::max({x0, x1, x2}))));
            const int top = std::max(0, int(std::floor(std::min({y0, y1, y2}))));
            const int bottom = std::min(size - 1, int(std::ceil(std::max({y0, y1, y2}))));
            if (left > right || top > bottom)
                continue;
            work += qint64(right - left + 1) * (bottom - top + 1);
            if (work > maxWork) {
                if (complete)
                    *complete = false;
                return image;
            }
            for (int y = top; y <= bottom; ++y) {
                for (int x = left; x <= right; ++x) {
                    const float px = x + 0.5f, py = y + 0.5f;
                    const float w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area;
                    const float w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area;
                    const float w2 = 1.f - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0)
                        continue;
                    const float z = w0 * a[2] + w1 * b[2] + w2 * c[2];
                    float &d = depth[size_t(y) * size + x];
                    if (z <= d)
                        continue;
                    d = z;
                    uchar *p = out + (qsizetype(y) * size + x) * 4;
                    p[0] = uchar(std::min(255.f, colour[0] * shade));
                    p[1] = uchar(std::min(255.f, colour[1] * shade));
                    p[2] = uchar(std::min(255.f, colour[2] * shade));
                }
            }
        }
    }
    return image;
}

std::vector<PreviewMesh> previewMeshes(const Model3D &model)
{
    std::vector<PreviewMesh> all, visible;
    for (const ModelPrimitive &prim : model.primitives) {
        PreviewMesh m;
        const Matrix4 matrix = model.meshMatrix(prim.mesh);
        m.positions.resize(size_t(prim.vertexCount()));
        for (size_t v = 0; v < m.positions.size(); ++v)
            m.positions[v] = transformPoint(matrix, &prim.positions[v * 3]);
        m.indices = prim.indices;
        if (!model.isAuxiliary(prim))
            visible.push_back(m);
        all.push_back(std::move(m));
    }
    return visible.empty() ? all : visible;
}
