#pragma once

#include <QString>

Qt::CaseSensitivity pathCaseSensitivity();
QString safeFileName(QString name);
bool isSafeRelativePath(const QString &path);
QString sanitizedRelativePath(const QString &path);
