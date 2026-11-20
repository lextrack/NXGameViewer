#include "core/records.h"
#include "core/registry.h"

#include <QByteArray>
#include <QString>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace {

constexpr int kMaxRows = 64;
constexpr int kMaxColumns = 32;
constexpr size_t kMaxEntries = 16;

const std::array<const char *, 20> kNames = {
    "fuzz.tbl",   "fuzz.json",   "fuzz.dat",  "fuzz.mdl",  "fuzz.bin",  "fuzz.txt",    "fuzz.bntx",
    "fuzz.xtx",   "fuzz.bfres",  "fuzz.byml", "fuzz.msbt", "fuzz.bcsv", "fuzz.bwav",   "fuzz.bars",
    "fuzz.opus",  "fuzz.sarc",   "fuzz.bundle", "fuzz.assets", "fuzz.acb", "fuzz.awb",
};

void exercise(const Document &doc, int depth);

void exerciseTable(const Table &table, int depth)
{
    (void)table.name();
    (void)table.description();
    const int rows = std::min(table.rowCount(), kMaxRows);
    const int columns = std::min(table.columnCount(), kMaxColumns);
    for (int c = 0; c < columns; ++c) {
        (void)table.columnName(c);
        (void)table.columnToolTip(c);
        (void)table.isNumeric(c);
    }
    for (int r = 0; r < rows; ++r) {
        (void)table.rowLabel(r);
        for (int c = 0; c < columns; ++c) {
            (void)table.text(r, c, DisplayMode::Auto);
            (void)table.text(r, c, DisplayMode::Hex);
            (void)table.sortKey(r, c);
            (void)table.toolTip(r, c);
        }
    }
    if (table.canExportJson())
        (void)table.exportJson();
    if (table.hasImage())
        (void)table.image();
    if (table.hasModel())
        (void)table.model();
    if (table.hasAudio()) {
        QString error;
        (void)table.decodeAudio(&error);
    }
    if (table.canChangeTextEncoding())
        if (auto other = table.withTextEncoding(TextEncoding::ShiftJis))
            if (other->rowCount() > 0 && other->columnCount() > 0)
                (void)other->text(0, 0, DisplayMode::Auto);
    if (auto records = table.recordSource()) {
        (void)records->recordTypeName();
        (void)records->guessFields();
        const int count = std::min(records->recordCount(), kMaxRows);
        for (int r = 0; r < count; ++r)
            (void)records->record(r);
    }
    if (auto archive = table.archive()) {
        const size_t count = std::min(archive->entries.size(), kMaxEntries);
        for (size_t i = 0; i < count; ++i) {
            (void)archive->entryHead(i, 64);
            const QByteArray bytes = archive->entryData(i);
            if (archive->extractor)
                (void)archive->extractor(i);
            if (depth == 0 && !bytes.isEmpty()) {
                static const FormatRegistry registry;
                QString error;
                if (auto inner = registry.openBytes(bytes, archive->entries[i].name, nullptr, &error))
                    exercise(*inner, depth + 1);
            }
        }
    }
}

void exercise(const Document &doc, int depth)
{
    for (const auto &table : doc.tables)
        if (table)
            exerciseTable(*table, depth);
}

}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const FormatRegistry registry;
    const QByteArray bytes(reinterpret_cast<const char *>(data), qsizetype(size));
    const QString name = QString::fromLatin1(kNames[size ? data[size - 1] % kNames.size() : 0]);
    for (const auto &reader : registry.readers()) {
        if (!reader->readsBytes())
            continue;
        QString error;
        if (auto doc = FormatRegistry::openBytesWith(*reader, bytes, name, &error))
            exercise(*doc, 0);
    }
    return 0;
}
