#pragma once

#include "formats/core/format.h"
#include "formats/models/model3d.h"

#include <QImage>
#include <QMatrix4x4>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QColor>
#include <QPoint>
#include <QPointF>
#include <QVector3D>

#include <memory>
#include <vector>

class QOpenGLBuffer;
class QPainter;
class QOpenGLShaderProgram;
class QOpenGLTexture;

class ModelView : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT

public:
    explicit ModelView(QWidget *parent = nullptr);
    ~ModelView() override;

    void setModel(std::shared_ptr<const Model3D> model, std::vector<std::shared_ptr<const Image>> textures);
    void setMessage(const QString &text);
    void clear();
    void resetView();
    bool failed() const { return m_failed; }
    QImage grabCleanImage();
    static bool isSupported();

signals:
    void renderingFailed();

protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    struct DrawCall
    {
        std::unique_ptr<QOpenGLBuffer> vertices;
        std::unique_ptr<QOpenGLBuffer> indices;
        std::unique_ptr<QOpenGLBuffer> edges;
        size_t primitive = 0;
        int indexCount = 0;
        int edgeCount = 0;
        int texture = -1;
        float alphaCutoff = 0.f;
        bool blend = false;
        bool cullBack = false;
    };

    void releaseGpu();
    void cleanupGl();
    void upload();
    void frame();
    void setViewAngles(float yaw, float pitch);
    struct GizmoHandle
    {
        int id = 0;
        bool positive = true;
        QPointF center;
        float depth = 0.f;
        QColor color;
        QString label;
        float yaw = 0.f;
        float pitch = 0.f;
    };
    static constexpr qreal kGizmoLength = 34;
    static constexpr qreal kGizmoDot = 9;

    void drawAxes(QPainter &painter) const;
    std::vector<GizmoHandle> gizmoHandles() const;
    QPointF gizmoCenter() const;
    int gizmoHandleAt(const QPointF &point) const;
    bool overGizmo(const QPointF &point) const;

    std::shared_ptr<const Model3D> m_model;
    std::vector<std::shared_ptr<const Image>> m_images;
    bool m_dirty = false;
    bool m_failed = false;
    QString m_message;

    std::unique_ptr<QOpenGLShaderProgram> m_program;
    std::vector<DrawCall> m_draws;
    std::vector<std::unique_ptr<QOpenGLTexture>> m_textures;
    std::unique_ptr<QOpenGLBuffer> m_grid;
    std::unique_ptr<QOpenGLBuffer> m_bones;
    int m_gridCount = 0;
    int m_boneCount = 0;
    bool m_wireframe = false;
    bool m_showSkeleton = false;
    bool m_showGrid = false;
    bool m_overlays = true;

    QVector3D m_center;
    float m_radius = 1.f;
    float m_floor = 0.f;
    float m_yaw = 30.f;
    float m_pitch = 15.f;
    float m_distance = 3.f;
    QVector3D m_pan;
    QPoint m_lastMouse;
    QPoint m_pressMouse;
    int m_pressHandle = -1;
    int m_hoverHandle = -1;
    bool m_hoverGizmo = false;
};
