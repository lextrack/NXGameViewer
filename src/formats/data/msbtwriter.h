#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <optional>

std::optional<QByteArray> msbtFromJson(const QJsonObject &edited, const QByteArray &original, QString *error);
