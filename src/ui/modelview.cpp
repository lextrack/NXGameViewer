#include "modelview.h"

#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QMouseEvent>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QKeyEvent>
#include <QPainter>
#include <QSurfaceFormat>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

const char *kVertexShader = R"(
attribute highp vec3 position;
attribute mediump vec3 normal;
attribute mediump vec2 uv;
uniform highp mat4 mvp;
varying mediump vec3 vNormal;
varying mediump vec2 vUv;
void main()
{
    vNormal = normal;
    vUv = uv;
    gl_Position = mvp * vec4(position, 1.0);
}
)";

const char *kFragmentShader = R"(
#ifdef GL_ES
precision mediump float;
#endif
uniform sampler2D tex;
uniform bool hasTexture;
uniform float alphaCutoff;
uniform bool blend;
uniform vec3 lightDir;
uniform vec3 baseColor;
uniform bool unlit;
uniform vec3 lineColor;
varying vec3 vNormal;
varying vec2 vUv;
void main()
{
    if (unlit) {
        gl_FragColor = vec4(lineColor, 1.0);
        return;
    }
    vec4 color = hasTexture ? texture2D(tex, vUv) : vec4(baseColor, 1.0);
    if (color.a < alphaCutoff)
        discard;
    float light = 0.45 + 0.55 * abs(dot(normalize(vNormal), lightDir));
    gl_FragColor = vec4(color.rgb * light, blend ? color.a : 1.0);
}
)";

constexpr int kStride = 8 * sizeof(float);

std::unique_ptr<QOpenGLBuffer> makeBuffer(QOpenGLBuffer::Type type, const void *data, qsizetype bytes)
{
    auto buffer = std::make_unique<QOpenGLBuffer>(type);
    buffer->create();
    buffer->bind();
    buffer->allocate(data, int(bytes));
    return buffer;
}

std::unique_ptr<QOpenGLBuffer> makeLines(const std::vector<QVector3D> &points)
{
    std::vector<float> vertices;
    vertices.reserve(points.size() * 8);
    for (const QVector3D &p : points)
        vertices.insert(vertices.end(), {p.x(), p.y(), p.z(), 0.f, 1.f, 0.f, 0.f, 0.f});
    return makeBuffer(QOpenGLBuffer::VertexBuffer, vertices.data(), qsizetype(vertices.size() * sizeof(float)));
}

QOpenGLTexture::WrapMode wrapMode(int mdlWrap)
{
    switch (mdlWrap) {
    case 1: return QOpenGLTexture::MirroredRepeat;
    case 2: return QOpenGLTexture::ClampToEdge;
    default: return QOpenGLTexture::Repeat;
    }
}

}

bool ModelView::isSupported()
{
    static const bool supported = [] {
        const QString platform = QGuiApplication::platformName();
        if (platform == QLatin1String("offscreen") || platform == QLatin1String("minimal"))
            return false;
        QOpenGLContext context;
        QOffscreenSurface surface;
        surface.create();
        return context.create() && context.makeCurrent(&surface);
    }();
    return supported;
}

ModelView::ModelView(QWidget *parent) : QOpenGLWidget(parent), m_failed(!isSupported())
{
    setMinimumHeight(120);
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(true);
    QSurfaceFormat surface = format();
    surface.setSamples(4);
    setFormat(surface);
    setToolTip(tr("Drag to turn, right-drag to move, wheel to zoom, double-click to reset\n"
                  "F: frame the model · 1, 3, 7: front, side, top view (with Ctrl: the opposite side)\n"
                  "W: wireframe · B: skeleton · G: ground grid · click an axis of the gizmo to look along it"));
}

ModelView::~ModelView()
{
    if (QOpenGLContext *gl = context())
        disconnect(gl, nullptr, this, nullptr);
    cleanupGl();
}

void ModelView::cleanupGl()
{
    if (!m_program && m_draws.empty() && m_textures.empty() && !m_grid && !m_bones)
        return;
    makeCurrent();
    releaseGpu();
    m_program.reset();
    doneCurrent();
    m_dirty = true;
}

void ModelView::setModel(std::shared_ptr<const Model3D> model, std::vector<std::shared_ptr<const Image>> textures)
{
    m_model = std::move(model);
    m_images = std::move(textures);
    m_message.clear();
    m_dirty = true;

    QVector3D lo(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    QVector3D hi = -lo;
    bool any = false;
    if (m_model) {
        for (const ModelPrimitive &prim : m_model->primitives) {
            if (m_model->isAuxiliary(prim))
                continue;
            const Matrix4 matrix = m_model->meshMatrix(prim.mesh);
            for (qsizetype v = 0; v < prim.vertexCount(); ++v) {
                const auto p = transformPoint(matrix, &prim.positions[size_t(v) * 3]);
                if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
                    continue;
                const QVector3D q(p[0], p[1], p[2]);
                lo = QVector3D(std::min(lo.x(), q.x()), std::min(lo.y(), q.y()), std::min(lo.z(), q.z()));
                hi = QVector3D(std::max(hi.x(), q.x()), std::max(hi.y(), q.y()), std::max(hi.z(), q.z()));
                any = true;
            }
        }
    }
    m_center = any ? (lo + hi) / 2 : QVector3D();
    m_floor = any ? lo.y() : 0.f;
    m_radius = any ? std::max(0.001f, (hi - lo).length() / 2) : 1.f;
    resetView();
}

void ModelView::setMessage(const QString &text)
{
    m_message = text;
    update();
}

void ModelView::clear()
{
    m_model.reset();
    m_images.clear();
    m_dirty = true;
    update();
}

void ModelView::resetView()
{
    m_yaw = 30.f;
    m_pitch = 15.f;
    frame();
}

QImage ModelView::grabCleanImage()
{
    m_overlays = false;
    const QImage image = grabFramebuffer();
    m_overlays = true;
    update();
    return image;
}

void ModelView::frame()
{
    m_distance = m_radius * 2.6f;
    m_pan = QVector3D();
    update();
}

void ModelView::setViewAngles(float yaw, float pitch)
{
    m_yaw = yaw;
    m_pitch = pitch;
    update();
}

void ModelView::initializeGL()
{
    initializeOpenGLFunctions();
    connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, &ModelView::cleanupGl, Qt::UniqueConnection);
    releaseGpu();
    m_program = std::make_unique<QOpenGLShaderProgram>();
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)
        || !m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader) || !m_program->link()) {
        qWarning("ModelView: %s", qPrintable(m_program->log()));
        m_failed = true;
        emit renderingFailed();
    }
    m_dirty = true;
}

void ModelView::releaseGpu()
{
    m_draws.clear();
    m_textures.clear();
    m_grid.reset();
    m_bones.reset();
    m_gridCount = m_boneCount = 0;
}

void ModelView::upload()
{
    releaseGpu();
    m_dirty = false;
    if (!m_model)
        return;

    m_textures.resize(m_images.size());
    for (size_t i = 0; i < m_images.size(); ++i) {
        const auto &image = m_images[i];
        if (!image || image->width <= 0 || image->height <= 0)
            continue;
        const QImage q(reinterpret_cast<const uchar *>(image->rgba.constData()), image->width, image->height,
                       qsizetype(image->width) * 4, QImage::Format_RGBA8888);
        auto texture = std::make_unique<QOpenGLTexture>(q);
        texture->setMinificationFilter(QOpenGLTexture::LinearMipMapLinear);
        texture->setMagnificationFilter(QOpenGLTexture::Linear);
        if (i < m_model->textures.size()) {
            texture->setWrapMode(QOpenGLTexture::DirectionS, wrapMode(m_model->textures[i].wrapU));
            texture->setWrapMode(QOpenGLTexture::DirectionT, wrapMode(m_model->textures[i].wrapV));
        }
        m_textures[i] = std::move(texture);
    }

    for (size_t index = 0; index < m_model->primitives.size(); ++index) {
        const ModelPrimitive &prim = m_model->primitives[index];
        if (m_model->isAuxiliary(prim) || prim.indices.empty())
            continue;
        const Matrix4 matrix = m_model->meshMatrix(prim.mesh);
        const qsizetype count = prim.vertexCount();
        std::vector<float> interleaved(size_t(count) * 8);
        for (qsizetype v = 0; v < count; ++v) {
            float *out = &interleaved[size_t(v) * 8];
            const auto p = transformPoint(matrix, &prim.positions[size_t(v) * 3]);
            out[0] = p[0];
            out[1] = p[1];
            out[2] = p[2];
            if (!prim.normals.empty()) {
                const float *n = &prim.normals[size_t(v) * 3];
                out[3] = matrix[0] * n[0] + matrix[4] * n[1] + matrix[8] * n[2];
                out[4] = matrix[1] * n[0] + matrix[5] * n[1] + matrix[9] * n[2];
                out[5] = matrix[2] * n[0] + matrix[6] * n[1] + matrix[10] * n[2];
            } else {
                out[3] = 0;
                out[4] = 1;
                out[5] = 0;
            }
            if (!prim.uv0.empty()) {
                out[6] = prim.uv0[size_t(v) * 2];
                out[7] = 1.f - prim.uv0[size_t(v) * 2 + 1];
            } else {
                out[6] = out[7] = 0;
            }
        }
        DrawCall d;
        d.primitive = index;
        d.vertices = makeBuffer(QOpenGLBuffer::VertexBuffer, interleaved.data(),
                                qsizetype(interleaved.size() * sizeof(float)));
        d.indices = makeBuffer(QOpenGLBuffer::IndexBuffer, prim.indices.data(),
                               qsizetype(prim.indices.size() * sizeof(quint32)));
        d.indexCount = int(prim.indices.size());
        if (prim.material >= 0 && size_t(prim.material) < m_model->materials.size()) {
            const ModelMaterial &material = m_model->materials[size_t(prim.material)];
            const int t = material.baseColorTexture;
            if (t >= 0 && size_t(t) < m_textures.size() && m_textures[size_t(t)]) {
                d.texture = t;
                d.alphaCutoff = material.alphaMode == AlphaMode::Mask ? material.alphaCutoff
                              : material.alphaMode == AlphaMode::Blend ? 0.01f : 0.f;
                d.blend = material.alphaMode == AlphaMode::Blend;
            }
            d.cullBack = !material.doubleSided;
        }
        m_draws.push_back(std::move(d));
    }
    std::stable_sort(m_draws.begin(), m_draws.end(), [](const DrawCall &a, const DrawCall &b) { return !a.blend && b.blend; });

    const float step = std::pow(10.f, std::floor(std::log10(std::max(m_radius * 0.2f, 1e-4f))));
    const int half = std::min(50, int(std::ceil(m_radius * 1.5f / step)));
    const float cx = std::round(m_center.x() / step) * step, cz = std::round(m_center.z() / step) * step;
    const float extent = float(half) * step;
    std::vector<QVector3D> grid;
    for (int i = -half; i <= half; ++i) {
        const float offset = float(i) * step;
        grid.insert(grid.end(), {QVector3D(cx + offset, m_floor, cz - extent), QVector3D(cx + offset, m_floor, cz + extent),
                                 QVector3D(cx - extent, m_floor, cz + offset), QVector3D(cx + extent, m_floor, cz + offset)});
    }
    m_grid = makeLines(grid);
    m_gridCount = int(grid.size());

    const std::vector<ModelNode> &nodes = m_model->nodes;
    std::vector<bool> inSkeleton(nodes.size(), false);
    for (const ModelMesh &mesh : m_model->meshes)
        for (int n : mesh.joints)
            for (; n >= 0 && size_t(n) < nodes.size() && !inSkeleton[size_t(n)]; n = nodes[size_t(n)].parent)
                inSkeleton[size_t(n)] = true;
    std::vector<QVector3D> bones;
    for (size_t n = 0; n < nodes.size(); ++n) {
        const int parent = nodes[n].parent;
        if (!inSkeleton[n] || parent < 0 || size_t(parent) >= nodes.size() || !inSkeleton[size_t(parent)])
            continue;
        const Matrix4 a = m_model->worldMatrix(parent), b = m_model->worldMatrix(int(n));
        bones.insert(bones.end(), {QVector3D(a[12], a[13], a[14]), QVector3D(b[12], b[13], b[14])});
    }
    if (!bones.empty()) {
        m_bones = makeLines(bones);
        m_boneCount = int(bones.size());
    }
}

void ModelView::paintGL()
{
    const QColor background(48, 52, 58);
    glClearColor(background.redF(), background.greenF(), background.blueF(), 1.f);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!m_failed && m_program) {
        if (m_dirty)
            upload();
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);

        QMatrix4x4 projection;
        const float aspect = height() > 0 ? float(width()) / float(height()) : 1.f;
        projection.perspective(35.f, aspect, std::max(m_distance * 0.01f, m_radius * 0.001f), m_distance + m_radius * 4.f);
        QMatrix4x4 view;
        view.translate(0, 0, -m_distance);
        view.rotate(m_pitch, 1, 0, 0);
        view.rotate(-m_yaw, 0, 1, 0);
        view.translate(-(m_center + m_pan));
        m_program->bind();
        m_program->setUniformValue("mvp", projection * view);
        m_program->setUniformValue("lightDir", (view.inverted().mapVector(QVector3D(0.3f, 0.5f, 0.8f))).normalized());
        m_program->setUniformValue("baseColor", QVector3D(0.78f, 0.74f, 0.68f));
        m_program->setUniformValue("tex", 0);
        m_program->setUniformValue("unlit", false);
        const int position = m_program->attributeLocation("position");
        const int normal = m_program->attributeLocation("normal");
        const int uv = m_program->attributeLocation("uv");
        m_program->enableAttributeArray(position);
        m_program->enableAttributeArray(normal);
        m_program->enableAttributeArray(uv);
        const auto bindVertices = [&](QOpenGLBuffer &buffer) {
            buffer.bind();
            m_program->setAttributeBuffer(position, GL_FLOAT, 0, 3, kStride);
            m_program->setAttributeBuffer(normal, GL_FLOAT, 3 * sizeof(float), 3, kStride);
            m_program->setAttributeBuffer(uv, GL_FLOAT, 6 * sizeof(float), 2, kStride);
        };
        const auto drawLines = [&](QOpenGLBuffer *buffer, int count, const QVector3D &color) {
            if (!buffer || count == 0)
                return;
            bindVertices(*buffer);
            m_program->setUniformValue("unlit", true);
            m_program->setUniformValue("lineColor", color);
            glDrawArrays(GL_LINES, 0, count);
            m_program->setUniformValue("unlit", false);
        };

        if (m_overlays && m_showGrid)
            drawLines(m_grid.get(), m_gridCount, QVector3D(0.36f, 0.39f, 0.43f));
        const bool wireframe = m_overlays && m_wireframe;
        if (wireframe) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(1.f, 1.f);
        }
        for (const DrawCall &d : m_draws) {
            if (d.blend) {
                glEnable(GL_BLEND);
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
                glDepthMask(GL_FALSE);
            } else {
                glDisable(GL_BLEND);
                glDepthMask(GL_TRUE);
            }
            if (d.cullBack)
                glEnable(GL_CULL_FACE);
            else
                glDisable(GL_CULL_FACE);
            bindVertices(*d.vertices);
            d.indices->bind();
            const bool textured = d.texture >= 0;
            m_program->setUniformValue("hasTexture", textured);
            m_program->setUniformValue("alphaCutoff", d.alphaCutoff);
            m_program->setUniformValue("blend", d.blend);
            if (textured)
                m_textures[size_t(d.texture)]->bind(0);
            glDrawElements(GL_TRIANGLES, d.indexCount, GL_UNSIGNED_INT, nullptr);
            if (textured)
                m_textures[size_t(d.texture)]->release(0);
        }
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        if (wireframe) {
            glDisable(GL_POLYGON_OFFSET_FILL);
            glDepthMask(GL_FALSE);
            m_program->setUniformValue("unlit", true);
            m_program->setUniformValue("lineColor", QVector3D(0.72f, 0.78f, 0.86f));
            for (DrawCall &d : m_draws) {
                if (!d.edges) {
                    const std::vector<quint32> &indices = m_model->primitives[d.primitive].indices;
                    std::vector<quint32> edges;
                    edges.reserve(indices.size() * 2);
                    for (size_t i = 0; i + 2 < indices.size(); i += 3)
                        edges.insert(edges.end(), {indices[i], indices[i + 1], indices[i + 1], indices[i + 2],
                                                   indices[i + 2], indices[i]});
                    d.edges = makeBuffer(QOpenGLBuffer::IndexBuffer, edges.data(),
                                         qsizetype(edges.size() * sizeof(quint32)));
                    d.edgeCount = int(edges.size());
                }
                bindVertices(*d.vertices);
                d.edges->bind();
                glDrawElements(GL_LINES, d.edgeCount, GL_UNSIGNED_INT, nullptr);
            }
            m_program->setUniformValue("unlit", false);
        }
        if (m_overlays && m_showSkeleton && m_boneCount > 0) {
            glDisable(GL_DEPTH_TEST);
            drawLines(m_bones.get(), m_boneCount, QVector3D(1.f, 0.78f, 0.25f));
        }
        m_program->disableAttributeArray(position);
        m_program->disableAttributeArray(normal);
        m_program->disableAttributeArray(uv);
        m_program->release();
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
    }
    if (!m_overlays)
        return;
    QPainter painter(this);
    if (!m_failed && m_program && m_model)
        drawAxes(painter);
    if (!m_message.isEmpty()) {
        painter.setPen(QColor(200, 200, 200));
        painter.drawText(rect(), Qt::AlignCenter, m_message);
    }
}

std::vector<ModelView::GizmoHandle> ModelView::gizmoHandles() const
{
    QMatrix4x4 rotation;
    rotation.rotate(m_pitch, 1, 0, 0);
    rotation.rotate(-m_yaw, 0, 1, 0);
    const QColor colors[3] = {QColor(255, 54, 83), QColor(138, 219, 0), QColor(44, 143, 255)};
    const QString names[3] = {QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")};
    const float views[6][2] = {{90.f, 0.f}, {-90.f, 0.f}, {0.f, 89.f}, {0.f, -89.f}, {0.f, 0.f}, {180.f, 0.f}};
    const QPointF center = gizmoCenter();
    std::vector<GizmoHandle> handles;
    for (int id = 0; id < 6; ++id) {
        QVector3D axis;
        axis[id / 2] = id % 2 == 0 ? 1.f : -1.f;
        const QVector3D d = rotation.mapVector(axis);
        GizmoHandle h;
        h.id = id;
        h.positive = id % 2 == 0;
        h.center = center + QPointF(d.x(), -d.y()) * kGizmoLength;
        h.depth = d.z();
        h.color = colors[id / 2];
        h.label = h.positive ? names[id / 2] : QStringLiteral("-") + names[id / 2];
        h.yaw = views[id][0];
        h.pitch = views[id][1];
        handles.push_back(h);
    }
    std::sort(handles.begin(), handles.end(), [](const GizmoHandle &x, const GizmoHandle &y) { return x.depth < y.depth; });
    return handles;
}

QPointF ModelView::gizmoCenter() const
{
    const qreal margin = kGizmoLength + kGizmoDot + 10;
    return QPointF(width() - margin, margin);
}

int ModelView::gizmoHandleAt(const QPointF &point) const
{
    const std::vector<GizmoHandle> handles = gizmoHandles();
    for (auto it = handles.rbegin(); it != handles.rend(); ++it) {
        const qreal radius = it->positive ? kGizmoDot : kGizmoDot - 1.5;
        if (QLineF(point, it->center).length() <= radius + 2)
            return it->id;
    }
    return -1;
}

bool ModelView::overGizmo(const QPointF &point) const
{
    return QLineF(point, gizmoCenter()).length() <= kGizmoLength + kGizmoDot + 4;
}

void ModelView::drawAxes(QPainter &painter) const
{
    const QPointF center = gizmoCenter();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    if (m_hoverGizmo) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 30));
        painter.drawEllipse(center, kGizmoLength + kGizmoDot + 4, kGizmoLength + kGizmoDot + 4);
    }
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(11);
    painter.setFont(font);
    const QColor background(48, 52, 58);
    const std::vector<GizmoHandle> handles = gizmoHandles();
    const auto shade = [&](const GizmoHandle &h) {
        if (h.id == m_hoverHandle)
            return h.color.lighter(125);
        const float k = 0.55f + 0.45f * (0.5f + 0.5f * h.depth);
        return QColor::fromRgbF(float(h.color.redF()) * k + float(background.redF()) * (1 - k),
                                float(h.color.greenF()) * k + float(background.greenF()) * (1 - k),
                                float(h.color.blueF()) * k + float(background.blueF()) * (1 - k));
    };
    for (const GizmoHandle &h : handles) {
        if (!h.positive)
            continue;
        painter.setPen(QPen(shade(h), 2.4, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(center, h.center);
    }
    for (const GizmoHandle &h : handles) {
        const bool hovered = h.id == m_hoverHandle;
        const QColor color = shade(h);
        if (h.positive) {
            painter.setPen(hovered ? QPen(Qt::white, 1.5) : Qt::NoPen);
            painter.setBrush(color);
            painter.drawEllipse(h.center, kGizmoDot, kGizmoDot);
            painter.setPen(QColor(16, 18, 22));
            painter.drawText(QRectF(h.center - QPointF(kGizmoDot, kGizmoDot), QSizeF(2 * kGizmoDot, 2 * kGizmoDot)),
                             Qt::AlignCenter, h.label);
        } else {
            const qreal radius = kGizmoDot - 1.5;
            QColor fill = color;
            fill.setAlpha(hovered ? 255 : 70);
            painter.setPen(QPen(color, 1.6));
            painter.setBrush(fill);
            painter.drawEllipse(h.center, radius, radius);
            if (hovered) {
                painter.setPen(QColor(16, 18, 22));
                painter.drawText(QRectF(h.center - QPointF(kGizmoDot, kGizmoDot), QSizeF(2 * kGizmoDot, 2 * kGizmoDot)),
                                 Qt::AlignCenter, h.label);
            }
        }
    }
}

void ModelView::mousePressEvent(QMouseEvent *event)
{
    m_lastMouse = event->position().toPoint();
    m_pressMouse = m_lastMouse;
    m_pressHandle = event->button() == Qt::LeftButton ? gizmoHandleAt(event->position()) : -1;
}

void ModelView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_pressHandle >= 0 && (event->position().toPoint() - m_pressMouse).manhattanLength() < 4
        && gizmoHandleAt(event->position()) == m_pressHandle) {
        for (const GizmoHandle &h : gizmoHandles())
            if (h.id == m_pressHandle)
                setViewAngles(h.yaw, h.pitch);
    }
    m_pressHandle = -1;
}

void ModelView::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint delta = event->position().toPoint() - m_lastMouse;
    m_lastMouse = event->position().toPoint();
    if (event->buttons() & Qt::LeftButton) {
        m_yaw -= float(delta.x()) * 0.4f;
        m_pitch = std::clamp(m_pitch + float(delta.y()) * 0.4f, -89.f, 89.f);
    } else if (event->buttons() & (Qt::RightButton | Qt::MiddleButton)) {
        QMatrix4x4 rotation;
        rotation.rotate(m_yaw, 0, 1, 0);
        rotation.rotate(-m_pitch, 1, 0, 0);
        const float scale = m_distance / float(std::max(1, height())) * 0.7f;
        m_pan += rotation.mapVector(QVector3D(-float(delta.x()) * scale, float(delta.y()) * scale, 0));
    } else {
        const bool over = overGizmo(event->position());
        const int handle = over ? gizmoHandleAt(event->position()) : -1;
        if (over == m_hoverGizmo && handle == m_hoverHandle)
            return;
        m_hoverGizmo = over;
        m_hoverHandle = handle;
        setCursor(handle >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
    update();
}

void ModelView::leaveEvent(QEvent *event)
{
    QOpenGLWidget::leaveEvent(event);
    if (!m_hoverGizmo && m_hoverHandle < 0)
        return;
    m_hoverGizmo = false;
    m_hoverHandle = -1;
    unsetCursor();
    update();
}

void ModelView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (gizmoHandleAt(event->position()) < 0)
        resetView();
}

void ModelView::keyPressEvent(QKeyEvent *event)
{
    const Qt::KeyboardModifiers modifiers = event->modifiers() & ~Qt::KeypadModifier;
    const bool opposite = modifiers == Qt::ControlModifier;
    const bool plain = modifiers == Qt::NoModifier;
    switch (event->key()) {
    case Qt::Key_1:
        if (!plain && !opposite)
            break;
        setViewAngles(opposite ? 180.f : 0.f, 0.f);
        return;
    case Qt::Key_3:
        if (!plain && !opposite)
            break;
        setViewAngles(opposite ? -90.f : 90.f, 0.f);
        return;
    case Qt::Key_7:
        if (!plain && !opposite)
            break;
        setViewAngles(0.f, opposite ? -89.f : 89.f);
        return;
    case Qt::Key_F:
        if (!plain)
            break;
        frame();
        return;
    case Qt::Key_W:
    case Qt::Key_B:
    case Qt::Key_G:
        if (!plain)
            break;
        (event->key() == Qt::Key_W ? m_wireframe : event->key() == Qt::Key_B ? m_showSkeleton : m_showGrid) ^= true;
        update();
        return;
    default:
        break;
    }
    QOpenGLWidget::keyPressEvent(event);
}

void ModelView::wheelEvent(QWheelEvent *event)
{
    const float steps = float(event->angleDelta().y()) / 120.f;
    m_distance = std::clamp(m_distance * std::pow(0.87f, steps), m_radius * 0.05f, m_radius * 40.f);
    update();
}
