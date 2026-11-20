#pragma once

#include <QByteArray>
#include <QJsonValue>
#include <QString>

#include <optional>

std::optional<QByteArray> bymlFromJson(const QJsonValue &edited, const QByteArray &original, QString *error);
