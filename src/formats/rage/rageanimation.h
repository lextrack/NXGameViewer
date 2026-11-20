#pragma once

#include "core/format.h"
#include "models/model3d.h"

#include <QByteArray>
#include <QString>

#include <optional>
#include <vector>

struct RageClipInfo
{
    QString name;
    int frames = 0;
    float seconds = 0;
    int tracks = 0;
    std::vector<int> boneIds;
};

bool isRageAnimationSet(QByteArrayView head);
std::optional<std::vector<RageClipInfo>> rageAnimationClips(const QByteArray &data, QString *error);
std::optional<ModelAnimation> readRageAnimationClip(const QByteArray &data, const QString &clip, const Model3D &model,
                                                    QString *error);

class RageAnimationReader : public FormatReader
{
public:
    static constexpr quint32 kType = 6;
    static constexpr qint64 kMaxFileSize = qint64{256} * 1024 * 1024;

    QString name() const override { return QStringLiteral("RAGE animation set"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;
};
