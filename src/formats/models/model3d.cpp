#include "models/model3d.h"

#include <cmath>

Matrix4 identityMatrix()
{
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

Matrix4 multiply(const Matrix4 &a, const Matrix4 &b)
{
    Matrix4 r{};
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            float sum = 0;
            for (int k = 0; k < 4; ++k)
                sum += a[size_t(k * 4 + row)] * b[size_t(col * 4 + k)];
            r[size_t(col * 4 + row)] = sum;
        }
    return r;
}

Matrix4 inverted(const Matrix4 &m, bool *ok)
{
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            a[r][c] = m[size_t(c * 4 + r)];
            a[r][c + 4] = r == c ? 1 : 0;
        }
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c]))
                pivot = r;
        if (std::fabs(a[pivot][c]) < 1e-12) {
            if (ok)
                *ok = false;
            return identityMatrix();
        }
        if (pivot != c)
            for (int k = 0; k < 8; ++k)
                std::swap(a[c][k], a[pivot][k]);
        const double d = a[c][c];
        for (int k = 0; k < 8; ++k)
            a[c][k] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == c)
                continue;
            const double f = a[r][c];
            for (int k = 0; k < 8; ++k)
                a[r][k] -= f * a[c][k];
        }
    }
    Matrix4 out{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[size_t(c * 4 + r)] = float(a[r][c + 4]);
    if (ok)
        *ok = true;
    return out;
}

std::array<float, 3> transformPoint(const Matrix4 &m, const float *p)
{
    return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}

std::array<float, 4> quaternionFromEulerZYX(float x, float y, float z)
{
    const double cx = std::cos(x / 2.0), sx = std::sin(x / 2.0);
    const double cy = std::cos(y / 2.0), sy = std::sin(y / 2.0);
    const double cz = std::cos(z / 2.0), sz = std::sin(z / 2.0);
    return {float(sx * cy * cz - cx * sy * sz), float(cx * sy * cz + sx * cy * sz),
            float(cx * cy * sz - sx * sy * cz), float(cx * cy * cz + sx * sy * sz)};
}

std::array<float, 4> multiplyQuaternions(const std::array<float, 4> &a, const std::array<float, 4> &b)
{
    const double ax = a[0], ay = a[1], az = a[2], aw = a[3];
    const double bx = b[0], by = b[1], bz = b[2], bw = b[3];
    return {float(aw * bx + ax * bw + ay * bz - az * by), float(aw * by - ax * bz + ay * bw + az * bx),
            float(aw * bz + ax * by - ay * bx + az * bw), float(aw * bw - ax * bx - ay * by - az * bz)};
}

std::array<float, 4> normalizedQuaternion(const std::array<float, 4> &q)
{
    const double length = std::sqrt(double(q[0]) * q[0] + double(q[1]) * q[1] + double(q[2]) * q[2] + double(q[3]) * q[3]);
    if (!std::isfinite(length) || length < 1e-6)
        return {0, 0, 0, 1};
    return {float(q[0] / length), float(q[1] / length), float(q[2] / length), float(q[3] / length)};
}

Matrix4 Model3D::localMatrix(int node) const
{
    const ModelNode &n = nodes.at(size_t(node));
    const float x = n.rotation[0], y = n.rotation[1], z = n.rotation[2], w = n.rotation[3];
    const float sx = n.scale[0], sy = n.scale[1], sz = n.scale[2];
    return {(1 - 2 * (y * y + z * z)) * sx, 2 * (x * y + z * w) * sx, 2 * (x * z - y * w) * sx, 0,
            2 * (x * y - z * w) * sy, (1 - 2 * (x * x + z * z)) * sy, 2 * (y * z + x * w) * sy, 0,
            2 * (x * z + y * w) * sz, 2 * (y * z - x * w) * sz, (1 - 2 * (x * x + y * y)) * sz, 0,
            n.translation[0], n.translation[1], n.translation[2], 1};
}

Matrix4 Model3D::worldMatrix(int node) const
{
    Matrix4 m = localMatrix(node);
    int guard = 0;
    for (int p = nodes.at(size_t(node)).parent; p >= 0 && guard < 1024; p = nodes.at(size_t(p)).parent, ++guard)
        m = multiply(localMatrix(p), m);
    return m;
}

Matrix4 Model3D::meshMatrix(int mesh) const
{
    const ModelMesh &m = meshes.at(size_t(mesh));
    if (!staticMeshesUseNodeTransform || !m.joints.empty() || m.node < 0)
        return identityMatrix();
    return worldMatrix(m.node);
}

bool Model3D::isAuxiliary(const ModelPrimitive &primitive) const
{
    if (primitive.environment)
        return true;
    if (primitive.mesh >= 0 && size_t(primitive.mesh) < meshes.size() && meshes[size_t(primitive.mesh)].auxiliary)
        return true;
    return primitive.material >= 0 && size_t(primitive.material) < materials.size()
        && materials[size_t(primitive.material)].auxiliary;
}
