#include "switchfs/switchkeys.h"

#include "core/crypto.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>

namespace {

constexpr qint64 kMaxKeyFile = qint64{4} * 1024 * 1024;

QHash<QString, QByteArray> parse(const QByteArray &text)
{
    QHash<QString, QByteArray> keys;
    for (QByteArray line : text.split('\n')) {
        line = line.trimmed();
        const qsizetype equals = line.indexOf('=');
        if (line.isEmpty() || line.startsWith(';') || line.startsWith('#') || equals <= 0)
            continue;
        const QString name = QString::fromLatin1(line.left(equals).trimmed()).toLower();
        const QByteArray value = QByteArray::fromHex(line.mid(equals + 1).trimmed());
        if (!value.isEmpty())
            keys.insert(name, value);
    }
    return keys;
}

QHash<QByteArray, QByteArray> parseTitleKeys(const QByteArray &text)
{
    QHash<QByteArray, QByteArray> out;
    const QHash<QString, QByteArray> titles = parse(text);
    for (auto it = titles.cbegin(); it != titles.cend(); ++it) {
        const QByteArray rightsId = QByteArray::fromHex(it.key().toLatin1());
        if (it.key().size() == 32 && rightsId.size() == 16 && it.value().size() == 16)
            out.insert(rightsId, it.value());
    }
    return out;
}

QString indexed(const char *name, int i)
{
    return QStringLiteral("%1_%2").arg(QLatin1String(name)).arg(i, 2, 16, QLatin1Char('0'));
}

QByteArray generateKek(const QByteArray &source, const QByteArray &masterKey, const QByteArray &kekSeed,
                       const QByteArray &keySeed)
{
    const QByteArray kek = crypto::ecbDecrypt(masterKey, kekSeed);
    const QByteArray sourceKek = crypto::ecbDecrypt(kek, source);
    return keySeed.isEmpty() ? sourceKek : crypto::ecbDecrypt(sourceKek, keySeed);
}

std::optional<QByteArray> readSmall(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return std::nullopt;
    }
    if (file.size() > kMaxKeyFile) {
        if (error)
            *error = QObject::tr("%1 is too large to be a key file.").arg(QFileInfo(path).fileName());
        return std::nullopt;
    }
    return file.readAll();
}

}

std::optional<SwitchKeys> SwitchKeys::load(const QString &prodKeys, QString *error)
{
    std::optional<QByteArray> prod = readSmall(prodKeys, error);
    if (!prod)
        return std::nullopt;
    QByteArray title;
    const QString titlePath = titleKeysBeside(prodKeys);
    if (!titlePath.isEmpty())
        title = readSmall(titlePath, nullptr).value_or(QByteArray());
    return fromText(*prod, title, error);
}

QString SwitchKeys::titleKeysBeside(const QString &prodKeys)
{
    const QString path = QFileInfo(prodKeys).dir().filePath(QStringLiteral("title.keys"));
    return QFileInfo::exists(path) ? path : QString();
}

std::optional<SwitchKeys> SwitchKeys::fromText(const QByteArray &prodKeys, const QByteArray &titleKeys, QString *error)
{
    const QHash<QString, QByteArray> keys = parse(prodKeys);
    SwitchKeys out;
    const QByteArray kekSeed = keys.value(QStringLiteral("aes_kek_generation_source"));
    const QByteArray keySeed = keys.value(QStringLiteral("aes_key_generation_source"));
    const bool canDerive = kekSeed.size() == 16 && keySeed.size() == 16;
    const char *areaNames[3] = {"key_area_key_application", "key_area_key_ocean", "key_area_key_system"};
    for (int g = 0; g < kGenerations; ++g) {
        const QByteArray master = keys.value(indexed("master_key", g));
        for (int k = 0; k < 3; ++k) {
            QByteArray key = keys.value(indexed(areaNames[k], g));
            const QByteArray source = keys.value(QStringLiteral("%1_source").arg(QLatin1String(areaNames[k])));
            if (key.size() != 16 && canDerive && master.size() == 16 && source.size() == 16)
                key = generateKek(source, master, kekSeed, keySeed);
            if (key.size() == 16)
                out.m_keyAreaKeys[size_t(g)][size_t(k)] = key;
        }
        QByteArray titlekek = keys.value(indexed("titlekek", g));
        const QByteArray titlekekSource = keys.value(QStringLiteral("titlekek_source"));
        if (titlekek.size() != 16 && master.size() == 16 && titlekekSource.size() == 16)
            titlekek = crypto::ecbDecrypt(master, titlekekSource);
        if (titlekek.size() == 16)
            out.m_titlekeks[size_t(g)] = titlekek;
    }
    out.m_headerKey = keys.value(QStringLiteral("header_key"));
    const QByteArray headerKekSource = keys.value(QStringLiteral("header_kek_source"));
    const QByteArray headerKeySource = keys.value(QStringLiteral("header_key_source"));
    const QByteArray master0 = keys.value(indexed("master_key", 0));
    if (out.m_headerKey.size() != 32 && canDerive && master0.size() == 16 && headerKekSource.size() == 16
        && headerKeySource.size() == 32)
        out.m_headerKey = crypto::ecbDecrypt(generateKek(headerKekSource, master0, kekSeed, keySeed), headerKeySource);
    if (out.m_headerKey.size() != 32) {
        if (error)
            *error = QObject::tr("The key file has no header_key (and not the keys to make one). Dump the keys of your "
                                 "own console again with a current tool.");
        return std::nullopt;
    }
    out.addTitleKeys(titleKeys);
    return out;
}

QByteArray SwitchKeys::keyAreaKey(int generation, int index) const
{
    if (generation < 0 || generation >= kGenerations || index < 0 || index > 2)
        return {};
    return m_keyAreaKeys[size_t(generation)][size_t(index)];
}

QByteArray SwitchKeys::titlekek(int generation) const
{
    if (generation < 0 || generation >= kGenerations)
        return {};
    return m_titlekeks[size_t(generation)];
}

void SwitchKeys::addTitleKey(const QByteArray &rightsId, const QByteArray &encryptedKey)
{
    if (rightsId.size() == 16 && encryptedKey.size() == 16)
        m_titleKeys.insert(rightsId, encryptedKey);
}

int SwitchKeys::addTitleKeys(const QByteArray &titleKeys)
{
    const QHash<QByteArray, QByteArray> titles = parseTitleKeys(titleKeys);
    for (auto it = titles.cbegin(); it != titles.cend(); ++it)
        m_titleKeys.insert(it.key(), it.value());
    return int(titles.size());
}

int SwitchKeys::countTitleKeys(const QByteArray &titleKeys)
{
    return int(parseTitleKeys(titleKeys).size());
}

int SwitchKeys::newestGeneration() const
{
    for (int g = kGenerations - 1; g >= 0; --g)
        if (!m_titlekeks[size_t(g)].isEmpty() || !m_keyAreaKeys[size_t(g)][0].isEmpty())
            return g;
    return -1;
}

QString SwitchKeys::generationName(int generation)
{
    return QStringLiteral("master_key_%1").arg(generation, 2, 16, QLatin1Char('0'));
}
