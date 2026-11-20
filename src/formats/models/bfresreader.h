#pragma once

#include "core/format.h"

#include <QStringList>

#include <optional>
#include <vector>

struct BfresClip
{
    QString name;
    int frames = 0;
    bool loop = false;
    QStringList bones;
    QStringList animatedBones;
};

class BfresReader : public FormatReader
{
public:
    static constexpr qint64 kMaxFileSize = qint64{1024} * 1024 * 1024;

    QString name() const override { return QStringLiteral("Switch model (BFRES)"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;
    bool readsBytes() const override { return true; }
    std::unique_ptr<Document> readBytes(const QByteArray &data, const QString &fileName, QString *error) const override;

    static bool isBfres(QByteArrayView head);
    static std::vector<BfresClip> skeletalClips(const QByteArray &data, QString *error = nullptr);
    static std::optional<ModelAnimation> readSkeletalClip(const QByteArray &data, const QString &clip,
                                                          const Model3D &model, QString *error = nullptr);
};
