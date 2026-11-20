#pragma once

#include "formats/core/format.h"
#include "formats/models/model3d.h"

#include <QMatrix4x4>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPoint>
#include <QVector3D>

#include <memory>
#include <vector>

class QOpenGLBuffer;
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
    static bool isSupported();

signals:
    void renderingFailed();

protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    struct DrawCall
    {
        std::unique_ptr<QOpenGLBuffer> vertices;
        std::unique_ptr<QOpenGLBuffer> indices;
        int indexCount = 0;
        int texture = -1;
        float alphaCutoff = 0.f;
        bool blend = false;
        bool cullBack = false;
    };

    void releaseGpu();
    void cleanupGl();
    void upload();

    std::shared_ptr<const Model3D> m_model;
    std::vector<std::shared_ptr<const Image>> m_images;
    bool m_dirty = false;
    bool m_failed = false;
    QString m_message;

    std::unique_ptr<QOpenGLShaderProgram> m_program;
    std::vector<DrawCall> m_draws;
    std::vector<std::unique_ptr<QOpenGLTexture>> m_textures;

    QVector3D m_center;
    float m_radius = 1.f;
    float m_yaw = 30.f;
    float m_pitch = 15.f;
    float m_distance = 3.f;
    QVector3D m_pan;
    QPoint m_lastMouse;
};
