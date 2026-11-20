#include "modelview.h"

#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QMouseEvent>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QPainter>
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
varying vec3 vNormal;
varying vec2 vUv;
void main()
{
    vec4 color = hasTexture ? texture2D(tex, vUv) : vec4(baseColor, 1.0);
    if (color.a < alphaCutoff)
        discard;
    float light = 0.45 + 0.55 * abs(dot(normalize(vNormal), lightDir));
    gl_FragColor = vec4(color.rgb * light, blend ? color.a : 1.0);
}
)";

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
    setToolTip(tr("Drag to turn, right-drag to move, wheel to zoom, double-click to reset"));
}

ModelView::~ModelView()
{
    if (QOpenGLContext *gl = context())
        disconnect(gl, nullptr, this, nullptr);
    cleanupGl();
}

void ModelView::cleanupGl()
{
    if (!m_program && m_draws.empty() && m_textures.empty())
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
    m_distance = m_radius * 2.6f;
    m_pan = QVector3D();
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

    for (const ModelPrimitive &prim : m_model->primitives) {
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
        d.vertices = std::make_unique<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
        d.vertices->create();
        d.vertices->bind();
        d.vertices->allocate(interleaved.data(), int(interleaved.size() * sizeof(float)));
        d.indices = std::make_unique<QOpenGLBuffer>(QOpenGLBuffer::IndexBuffer);
        d.indices->create();
        d.indices->bind();
        d.indices->allocate(prim.indices.data(), int(prim.indices.size() * sizeof(quint32)));
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
}

void ModelView::paintGL()
{
    const QColor background(48, 52, 58);
    glClearColor(background.redF(), background.greenF(), background.blueF(), 1.f);
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
        const int position = m_program->attributeLocation("position");
        const int normal = m_program->attributeLocation("normal");
        const int uv = m_program->attributeLocation("uv");
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
            d.vertices->bind();
            d.indices->bind();
            m_program->enableAttributeArray(position);
            m_program->enableAttributeArray(normal);
            m_program->enableAttributeArray(uv);
            m_program->setAttributeBuffer(position, GL_FLOAT, 0, 3, 8 * sizeof(float));
            m_program->setAttributeBuffer(normal, GL_FLOAT, 3 * sizeof(float), 3, 8 * sizeof(float));
            m_program->setAttributeBuffer(uv, GL_FLOAT, 6 * sizeof(float), 2, 8 * sizeof(float));
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
        m_program->disableAttributeArray(position);
        m_program->disableAttributeArray(normal);
        m_program->disableAttributeArray(uv);
        m_program->release();
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
    }
    if (!m_message.isEmpty()) {
        QPainter painter(this);
        painter.setPen(QColor(200, 200, 200));
        painter.drawText(rect(), Qt::AlignCenter, m_message);
    }
}

void ModelView::mousePressEvent(QMouseEvent *event)
{
    m_lastMouse = event->position().toPoint();
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
        return;
    }
    update();
}

void ModelView::mouseDoubleClickEvent(QMouseEvent *)
{
    resetView();
}

void ModelView::wheelEvent(QWheelEvent *event)
{
    const float steps = float(event->angleDelta().y()) / 120.f;
    m_distance = std::clamp(m_distance * std::pow(0.87f, steps), m_radius * 0.05f, m_radius * 40.f);
    update();
}
