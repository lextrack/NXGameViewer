#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QVector>

#include <optional>

#include "core/textdecode.h"

enum class ColumnType { String, U32, Byte };

struct TblColumn
{
    int offset = 0;
    ColumnType type = ColumnType::U32;
};

struct TblSection
{
    QString name;
    quint32 hash = 0;
    quint32 dataOffset = 0;
    quint32 entrySize = 0;
    quint32 entryCount = 0;
    QVector<TblColumn> columns;
};

class TblFile
{
public:
    static constexpr qint64 kMaxFileSize = qint64{256} * 1024 * 1024;
    static constexpr quint32 kMaxEntrySize = 64 * 1024;

    bool load(const QByteArray &data, QString *error = nullptr);
    bool loadFromFile(const QString &path, QString *error = nullptr);

    const QVector<TblSection> &sections() const { return m_sections; }
    qsizetype fileSize() const { return m_data.size(); }
    qsizetype stringPoolOffset() const { return m_poolStart; }

    QByteArrayView entry(int section, int row) const;

    std::optional<QString> textAt(quint64 offset) const;
    TextEncoding textEncoding() const { return m_encoding; }

private:
    quint64 u64At(qsizetype pos) const;
    void inferColumns(TblSection &section) const;

    QByteArray m_data;
    qsizetype m_poolStart = 0;
    TextEncoding m_encoding = TextEncoding::Utf8;
    QVector<TblSection> m_sections;
};
