#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

bool canApplyEditedJson(const QByteArray &leaf);
std::optional<QByteArray> applyEditedJson(const QByteArray &leaf, const QByteArray &json, QString *error);
