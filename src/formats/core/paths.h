#pragma once

#include <QString>

QString safeFileName(QString name);
bool isSafeRelativePath(const QString &path);
QString sanitizedRelativePath(const QString &path);
