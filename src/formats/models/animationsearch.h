#pragma once

#include "models/model3d.h"

#include <QString>

#include <vector>

struct AnimationFile
{
    QString path;
    QString name;
    int matchedBones = 0;
    int bones = 0;
    QString clip;
    QString entry;
    int frames = 0;

    bool sameAnimation(const AnimationFile &other) const
    {
        return path == other.path && clip == other.clip && entry == other.entry;
    }
};

std::vector<AnimationFile> inspectAnimationFile(const QString &path, const Model3D &model);
std::vector<AnimationFile> findAnimationFiles(const QString &modelPath, const Model3D &model);
std::vector<ModelAnimation> loadAnimations(const std::vector<AnimationFile> &animations, const Model3D &model,
                                           QStringList *problems = nullptr);
