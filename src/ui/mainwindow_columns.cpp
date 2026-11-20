#include "mainwindow.h"
#include "formats/data/bcsvreader.h"
#include "core/paths.h"

#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QTableView>

#include <memory>

namespace {

constexpr auto kNewGameMarker = "new-game";

}

QString MainWindow::columnsFolder() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/columns");
}

void MainWindow::loadHashNames()
{
    const QFileInfo info(QDir(columnsFolder()).filePath(QStringLiteral("hash names.txt")));
    const QDateTime time = info.exists() ? info.lastModified() : QDateTime();
    if (time == m_hashNamesTime)
        return;
    m_hashNamesTime = time;
    QStringList lines;
    QFile file(info.filePath());
    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
        lines = QString::fromUtf8(file.readAll()).split(u'\n');
    BcsvReader::setHashNames(lines);
}

void MainWindow::loadGameProfiles(const QString &select)
{
    QStringList games;
    for (const QFileInfo &f : QDir(columnsFolder()).entryInfoList({"*.json"}, QDir::Files, QDir::Name))
        games << f.completeBaseName();
    if (games.isEmpty())
        games << tr("Default");

    {
        const QSignalBlocker blocker(m_gameBox);
        m_gameBox->clear();
        m_gameBox->addItems(games);
        m_gameBox->insertSeparator(m_gameBox->count());
        m_gameBox->addItem(tr("New game…"), QString::fromLatin1(kNewGameMarker));
    }
    selectGame(games.contains(select) ? select : games.first());
}

void MainWindow::selectGame(const QString &name)
{
    {
        const QSignalBlocker blocker(m_gameBox);
        m_gameBox->setCurrentIndex(m_gameBox->findText(name));
    }
    QString error;
    QStringList warnings;
    if (!m_columns.load(QDir(columnsFolder()).filePath(name + ".json"), &error, &warnings))
        QMessageBox::warning(this, tr("Column file"), error);
    else if (!warnings.isEmpty())
        QMessageBox::warning(this, tr("Column file"), warnings.join('\n'));
    QSettings().setValue("game", name);
    rebuildShownTable(true);
}

void MainWindow::createGame()
{
    bool ok = false;
    const QString name = safeFileName(QInputDialog::getText(this, tr("New game"),
                                                            tr("Game name (for its column names):"),
                                                            QLineEdit::Normal, {}, &ok));
    if (!ok || name.isEmpty()) {
        loadGameProfiles(m_columns.gameName());
        return;
    }
    SchemaStore store;
    store.setPath(QDir(columnsFolder()).filePath(name + ".json"));
    store.setGameName(name);
    QString error;
    if (!QFileInfo::exists(store.path()) && !store.save(&error))
        QMessageBox::warning(this, tr("New game"), error);
    loadGameProfiles(name);
}

void MainWindow::saveColumns()
{
    QString error;
    if (!m_columns.save(&error))
        QMessageBox::warning(this, tr("Could not save column names"), error);
    rebuildShownTable(true);
}

void MainWindow::showColumnMenu(const QPoint &pos)
{
    const auto *table = dynamic_cast<const RecordTable *>(m_shownTable.get());
    const int column = m_table->horizontalHeader()->logicalIndexAt(pos);
    if (!table || column < 0)
        return;

    const std::shared_ptr<const RecordSource> records = table->recordSource();
    const QString recordType = records->recordTypeName();
    const int recordSize = records->recordSize();
    const FieldDef field = table->layout().at(size_t(column));

    QMenu menu(this);
    menu.setToolTipsVisible(true);
    menu.addSection(RecordTable::defaultLabel(field) + (field.name.isEmpty() ? QString() : "  " + field.name));
    QAction *rename = menu.addAction(tr("Rename…"));
    rename->setToolTip(tr("Give this column a name. It is remembered for every file of this kind"));
    QMenu *typeMenu = menu.addMenu(tr("Type"));
    typeMenu->menuAction()->setToolTip(tr("Choose how the bytes of this column are read, for example as a number or as text"));
    for (FieldType type : allFieldTypes()) {
        if (type == FieldType::Text && !records->hasTextPointers())
            continue;
        if (type == FieldType::Chars && field.type != FieldType::Chars)
            continue;
        const int size = type == FieldType::Chars ? field.length() : fieldSize(type);
        QAction *a = typeMenu->addAction(QStringLiteral("%1  (%2 bytes)").arg(fieldTypeName(type)).arg(size));
        a->setCheckable(true);
        a->setChecked(type == field.type);
        a->setEnabled(field.offset + size <= recordSize);
        a->setData(int(type));
    }
    menu.addSeparator();
    QAction *reset = menu.addAction(tr("Reset this column"));
    reset->setToolTip(tr("Forget the name and type you gave this column"));
    reset->setEnabled(field.userDefined);
    QAction *resetAll = menu.addAction(tr("Reset all columns of this table"));
    resetAll->setToolTip(tr("Forget every column name and type you gave this table"));
    resetAll->setEnabled(!m_columns.fields(m_currentFormat, recordType, recordSize).empty());

    QAction *chosen = menu.exec(m_table->horizontalHeader()->mapToGlobal(pos));
    if (!chosen)
        return;

    if (chosen == rename) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Rename column"),
                                                   tr("Name for %1:").arg(RecordTable::defaultLabel(field)),
                                                   QLineEdit::Normal, field.name, &ok);
        if (!ok)
            return;
        FieldDef renamed = field;
        renamed.name = name.trimmed();
        m_columns.setField(m_currentFormat, recordType, recordSize, renamed);
    } else if (chosen == reset) {
        m_columns.removeField(m_currentFormat, recordType, recordSize, field.offset);
    } else if (chosen == resetAll) {
        m_columns.clearTable(m_currentFormat, recordType, recordSize);
    } else if (chosen->data().isValid()) {
        FieldDef retyped = field;
        retyped.type = FieldType(chosen->data().toInt());
        m_columns.setField(m_currentFormat, recordType, recordSize, retyped);
    } else {
        return;
    }
    saveColumns();
}

void MainWindow::gameChosen(int index)
{
    if (m_gameBox->itemData(index).toString() == QLatin1String(kNewGameMarker))
        createGame();
    else
        selectGame(m_gameBox->itemText(index));
}
