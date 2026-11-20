#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>

#include <array>
#include <optional>

class SwitchKeys
{
public:
    static constexpr int kGenerations = 32;

    static std::optional<SwitchKeys> load(const QString &prodKeys, QString *error);
    static std::optional<SwitchKeys> fromText(const QByteArray &prodKeys, const QByteArray &titleKeys, QString *error);
    static QString titleKeysBeside(const QString &prodKeys);

    QByteArray headerKey() const { return m_headerKey; }
    QByteArray keyAreaKey(int generation, int index) const;
    QByteArray titlekek(int generation) const;
    QByteArray titleKey(const QByteArray &rightsId) const { return m_titleKeys.value(rightsId); }
    void addTitleKey(const QByteArray &rightsId, const QByteArray &encryptedKey);
    int addTitleKeys(const QByteArray &titleKeys);
    int titleKeyCount() const { return int(m_titleKeys.size()); }
    int newestGeneration() const;
    static int countTitleKeys(const QByteArray &titleKeys);
    static QString generationName(int generation);

private:
    QByteArray m_headerKey;
    std::array<std::array<QByteArray, 3>, kGenerations> m_keyAreaKeys;
    std::array<QByteArray, kGenerations> m_titlekeks;
    QHash<QByteArray, QByteArray> m_titleKeys;
};
