#pragma once

#include "core/format.h"

class FormatRegistry
{
public:
    explicit FormatRegistry(std::vector<std::unique_ptr<FormatReader>> extraReaders = {});

    const std::vector<std::unique_ptr<FormatReader>> &readers() const { return m_readers; }
    const FormatReader *byName(const QString &name) const;

    std::vector<const FormatReader *> candidates(const QString &path) const;
    std::vector<const FormatReader *> candidates(const QString &fileName, QByteArrayView head, bool bytesOnly) const;

    std::unique_ptr<Document> open(const QString &path, const FormatReader **usedReader, QString *error) const;

    std::unique_ptr<Document> openBytes(const QByteArray &data, const QString &fileName,
                                        const FormatReader **usedReader, QString *error) const;

    static std::unique_ptr<Document> readWith(const FormatReader &reader, const QString &path, QString *error);

    std::unique_ptr<Document> openEntry(const Archive &archive, size_t index, const FormatReader **usedReader,
                                        QString *error) const;
    static std::unique_ptr<Document> openEntryWith(const FormatReader &reader, const Archive &archive, size_t index,
                                                   QString *error);

    static std::unique_ptr<Document> openWith(const FormatReader &reader, const QString &path, QString *error);
    static std::unique_ptr<Document> openBytesWith(const FormatReader &reader, const QByteArray &data,
                                                   const QString &fileName, QString *error);

private:
    std::vector<const FormatReader *> scored(const QString &fileName, QByteArrayView head, bool bytesOnly) const;
    std::vector<const FormatReader *> storedReaders(const QString &fileName, QByteArrayView head) const;

    std::vector<std::unique_ptr<FormatReader>> m_readers;
};
