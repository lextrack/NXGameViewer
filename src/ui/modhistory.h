#pragma once

#include "formats/modding/modlibrary.h"

#include <QSettings>
#include <QVariantList>

#include <vector>

inline constexpr auto kModHistorySetting = "mods/history";
inline constexpr auto kLegacyModFolderSetting = "modFolder";

inline std::vector<ModRecord> loadModHistory()
{
    QSettings settings;
    std::vector<ModRecord> history;
    for (const QVariant &v : settings.value(kModHistorySetting).toList())
        if (std::optional<ModRecord> r = modRecordFromVariant(v.toMap()))
            history.push_back(std::move(*r));
    const QString legacy = settings.value(kLegacyModFolderSetting).toString();
    if (!legacy.isEmpty()) {
        ModRecord r = modRecord(legacy);
        if (r.exists())
            rememberMod(history, std::move(r));
        settings.remove(kLegacyModFolderSetting);
        QVariantList list;
        for (const ModRecord &record : history)
            list << modRecordToVariant(record);
        settings.setValue(kModHistorySetting, list);
    }
    return history;
}

inline void saveModHistory(const std::vector<ModRecord> &history)
{
    QVariantList list;
    for (const ModRecord &r : history)
        list << modRecordToVariant(r);
    QSettings().setValue(kModHistorySetting, list);
}

inline void rememberModInHistory(ModRecord record)
{
    std::vector<ModRecord> history = loadModHistory();
    rememberMod(history, std::move(record));
    saveModHistory(history);
}
