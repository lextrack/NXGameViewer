#pragma once

#include "core/format.h"

class SwitchPackageReader : public FormatReader
{
public:
    QString name() const override { return QStringLiteral("Switch game package (NSP/XCI/NUP)"); }
    int score(const QString &fileName, QByteArrayView head) const override;
    std::unique_ptr<Document> read(const QString &path, QString *error) const override;

    static void setKeyFile(const QString &prodKeys);
    static QString keyFile();
    static bool isPackageName(const QString &fileName);
    static bool needsKeys(const QString &path);
};
