#pragma once

#include <QByteArray>
#include <QString>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class SwitchKeys;

class SwitchStorage
{
public:
    virtual ~SwitchStorage() = default;
    virtual qint64 size() const = 0;
    virtual bool read(qint64 offset, char *out, qint64 length) const = 0;
    virtual bool decryptedSections() const { return false; }
    virtual bool seeksFast() const { return true; }
    QByteArray read(qint64 offset, qint64 length) const;
};

using StoragePtr = std::shared_ptr<const SwitchStorage>;

StoragePtr fileStorage(const QString &path, QString *error);
StoragePtr bytesStorage(QByteArray data);
StoragePtr subStorage(const StoragePtr &base, qint64 offset, qint64 size);

struct PartitionEntry
{
    QString name;
    qint64 offset = 0;
    qint64 size = 0;
};

std::optional<std::vector<PartitionEntry>> readPartition(const StoragePtr &storage, QString *error);

struct RomfsFile
{
    QString path;
    qint64 offset = 0;
    qint64 size = 0;
};

std::optional<std::vector<RomfsFile>> listRomfs(const StoragePtr &romfs, QString *error);

class SwitchNca
{
public:
    enum ContentType { Program = 0, Meta = 1, Control = 2, Manual = 3, Data = 4, PublicData = 5 };

    static std::optional<SwitchNca> open(const StoragePtr &storage, const SwitchKeys &keys, QString *error);

    quint64 titleId() const { return m_titleId; }
    int keyGeneration() const { return m_generation; }
    bool hasGenerationKeys() const { return m_hasGenerationKeys; }
    bool hasSection(int section) const;
    bool isPatchSection(int section) const;
    bool isRomfsSection(int section) const;
    bool isPartitionSection(int section) const;
    StoragePtr partition(int section, QString *error) const;
    StoragePtr romfs(int section, QString *error) const;
    bool verifyRomfs(int section, const std::function<bool(qint64 done, qint64 total)> &progress, QString *error) const;

private:
    QByteArray fsHeader(int section) const { return m_header.mid(0x400 + 0x200 * section, 0x200); }
    StoragePtr sectionData(int section, QString *error) const;

    StoragePtr m_storage;
    QByteArray m_header;
    QByteArray m_key;
    QString m_keyProblem;
    quint64 m_titleId = 0;
    int m_generation = 0;
    bool m_hasGenerationKeys = true;
};
