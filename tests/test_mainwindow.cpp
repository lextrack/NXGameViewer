#include "ui/animationdialog.h"
#include "ui/batchexportdialog.h"
#include "ui/fasticonprovider.h"
#include "ui/mainwindow.h"
#include "ui/contenticons.h"
#include "ui/tablemodel.h"
#include "formats/core/registry.h"
#include "formats/core/schema.h"
#include "formats/archives/compress.h"
#include "formats/archives/sarcwriter.h"
#include "formats/textures/xtxreader.h"
#include "sample_bntx.h"
#include "sample_mdl.h"
#include "sample_sarc.h"
#include "sample_rpf6.h"
#include "sample_tbl.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDialogButtonBox>
#include <QDropEvent>
#include <QFile>
#include <QtEndian>
#include <QImage>
#include <QFileDialog>
#include <QMimeData>
#include <QSaveFile>
#include <QStatusBar>
#include <QMouseEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QProgressDialog>
#include <QMessageBox>
#include <QFileSystemModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QMap>
#include <QMenu>
#include <QTimer>
#include <QMenuBar>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QStackedWidget>
#include <QTableView>
#include <QSettings>
#include <QTreeWidget>
#include "ui/gameextractdialog.h"
#include "ui/modhistory.h"
#include "ui/modmanagerdialog.h"
#include "formats/switchfs/switchpackagereader.h"
#include "switch_samples.h"
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

class TestMainWindow : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    // Sends the same events a real drag from the file manager would.
    bool drop(MainWindow &w, const QString &path)
    {
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(path)});
        const QPoint pos = w.rect().center();

        QDragEnterEvent enter(QPointF(pos), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&w, &enter);
        if (!enter.isAccepted())
            return false;

        QDropEvent dropEvent(QPointF(pos), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&w, &dropEvent);
        return dropEvent.isAccepted();
    }

    int page(MainWindow &w) { return w.findChild<QStackedWidget *>()->currentIndex(); }

    static QByteArray makeXtx()
    {
        auto u32 = [](QByteArray &b, quint32 v) {
            char raw[4];
            qToLittleEndian(v, raw);
            b.append(raw, 4);
        };
        auto u64 = [](QByteArray &b, quint64 v) {
            char raw[8];
            qToLittleEndian(v, raw);
            b.append(raw, 8);
        };
        QByteArray xtx("DFvN", 4);
        u32(xtx, 0x10); u32(xtx, 1); u32(xtx, 1);
        xtx.append("HBvN", 4); u32(xtx, 0x24); u64(xtx, 0x78); u64(xtx, 0x24); u32(xtx, 2); u32(xtx, 0); u32(xtx, 0);
        QByteArray info;
        u64(info, 512); u32(info, 512); u32(info, 8); u32(info, 8); u32(info, 1); u32(info, 1); u32(info, 0x25);
        u32(info, 1); u32(info, 512);
        info.append(QByteArray(17 * 4, '\0'));
        u32(info, 0); u32(info, 0x10007); u32(info, 0);
        xtx.append(info);
        xtx.append("HBvN", 4); u32(xtx, 0x24); u64(xtx, 512); u64(xtx, 0x24); u32(xtx, 3); u32(xtx, 1); u32(xtx, 0);
        xtx.append(QByteArray(512, '\0'));
        return xtx;
    }


private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);  // don't touch the real app settings
        QSettings().remove("batchExport");
        QSettings().remove("extract");
        QFile f(m_dir.filePath("items.tbl"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(sample::makeSample());
    }

    void startsEmpty()
    {
        MainWindow w;
        QCOMPARE(page(w), 0);  // the "open a folder or a file" page
    }

    void dropFolderOpensIt()
    {
        MainWindow w;
        QVERIFY(drop(w, m_dir.path()));
        QVERIFY(w.windowTitle().contains(m_dir.path()));
    }

    void dropFileOpensItsFolderAndTheFile()
    {
        MainWindow w;
        QVERIFY(drop(w, m_dir.filePath("items.tbl")));
        QVERIFY(w.windowTitle().contains(m_dir.path()));
        QTRY_COMPARE(page(w), 1);  // the table page, once read in the background
    }

    void fileListFollowsChangesOnDisk()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("sub"));
        MainWindow w;
        w.show();
        QVERIFY(drop(w, dir.path()));
        auto *tree = w.findChild<QTreeView *>();
        auto *model = qobject_cast<QFileSystemModel *>(tree->model());
        QVERIFY(model);
        QTRY_COMPARE(model->rowCount(tree->rootIndex()), 1);
        {
            QFile f(dir.filePath("added.bin"));
            QVERIFY(f.open(QIODevice::WriteOnly));
        }
        QTRY_COMPARE(model->rowCount(tree->rootIndex()), 2);
        QVERIFY(QFile::remove(dir.filePath("added.bin")));
        QTRY_COMPARE(model->rowCount(tree->rootIndex()), 1);
    }

    void openFileReloadsWhenItChangesOnDisk()
    {
        auto texture = [](const QByteArray &name) {
            samplebntx::Texture t;
            t.name = name;
            t.linear = QByteArray(8 * 8 * 4, '\x7f');
            return t;
        };
        auto write = [](const QString &path, const QByteArray &data) {
            QSaveFile f(path);
            return f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.commit();
        };
        QTemporaryDir dir;
        const QString path = dir.filePath("icons.bntx");
        QVERIFY(write(path, samplebntx::make({texture("First"), texture("Second")})));

        MainWindow w;
        w.show();
        QVERIFY(drop(w, path));
        QComboBox *tables = nullptr;
        QTRY_VERIFY([&] {
            for (QComboBox *box : w.findChildren<QComboBox *>())
                if (box->findText("Second", Qt::MatchStartsWith) >= 0)
                    tables = box;
            return tables != nullptr;
        }());
        tables->setCurrentIndex(1);

        QVERIFY(write(path, samplebntx::make({texture("First"), texture("Renamed")})));
        QTRY_VERIFY(tables->findText("Renamed", Qt::MatchStartsWith) == 1);
        QCOMPARE(tables->currentIndex(), 1);
        QTRY_VERIFY(w.statusBar()->currentMessage().contains("changed on disk"));

        QVERIFY(write(path, samplebntx::make({texture("First"), texture("Again")})));
        QAction *reload = nullptr;
        for (QAction *a : w.findChildren<QAction *>())
            if (a->text() == "Reload")
                reload = a;
        QVERIFY(reload && reload->isEnabled());
        reload->trigger();
        QTRY_VERIFY(tables->findText("Again", Qt::MatchStartsWith) == 1);
        QCOMPARE(tables->currentIndex(), 1);

        QVERIFY(QFile::remove(path));
        QTRY_VERIFY(w.statusBar()->currentMessage().contains("no longer on disk"));
        QCOMPARE(tables->findText("Again", Qt::MatchStartsWith), 1);
    }

    void modelKeepsTableAlive()
    {
        FormatRegistry registry;
        QString error;
        std::shared_ptr<const Document> doc = registry.open(m_dir.filePath("items.tbl"), nullptr, &error);
        QVERIFY(doc);
        std::shared_ptr<const Table> table = doc->tables.at(0);
        const std::weak_ptr<const Table> watcher = table;

        TableModel model;
        model.setTable(table);
        table.reset();
        doc.reset();                  // the window lets go of everything...
        QVERIFY(!watcher.expired());  // ...but the model still owns the table it shows
        QCOMPARE(model.index(1, 0).data().toString(), QString("Shield"));

        model.clear();
        QVERIFY(watcher.expired());  // freed as soon as nobody shows it
    }

    void savedColumnNamesAreShown()
    {
        // A column file for the "Default" game, as the right-click menu would save it.
        SchemaStore columns;
        columns.setPath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/columns/Default.json");
        columns.setField("Falcom TBL", "ItemData", 16, {0, FieldType::Text, "name", true});
        columns.setField("Falcom TBL", "ItemData", 16, {8, FieldType::U16, "price", true});
        QString error;
        QVERIFY2(columns.save(&error), qPrintable(error));

        MainWindow w;
        QVERIFY(drop(w, m_dir.filePath("items.tbl")));
        const QAbstractItemModel *model = w.findChild<QTableView *>()->model();
        QTRY_COMPARE(model->headerData(0, Qt::Horizontal).toString(), QString("name"));
        QCOMPARE(model->headerData(1, Qt::Horizontal).toString(), QString("price"));
        QCOMPARE(model->headerData(2, Qt::Horizontal).toString(), QString("+0x0A u8"));  // leftover bytes
        QCOMPARE(model->index(0, 1).data().toString(), QString("42"));

        QFile::remove(columns.path());
    }

    void switchingGamesSwitchesColumnNames()
    {
        const QString folder = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/columns/";
        for (const auto &[game, name] : {std::pair{"Alpha", "alpha name"}, std::pair{"Beta", "beta name"}}) {
            SchemaStore columns;
            columns.setPath(folder + game + ".json");
            columns.setGameName(game);
            columns.setField("Falcom TBL", "ItemData", 16, {0, FieldType::Text, name, true});
            QString error;
            QVERIFY2(columns.save(&error), qPrintable(error));
        }
        MainWindow w;
        QComboBox *game = nullptr;
        for (QComboBox *box : w.findChildren<QComboBox *>())
            if (box->toolTip().startsWith("Column names"))
                game = box;
        QVERIFY(game);
        QVERIFY(game->findText("Alpha") >= 0 && game->findText("Beta") >= 0);
        QVERIFY(game->findText("New game…") >= 0);
        QVERIFY(drop(w, m_dir.filePath("items.tbl")));
        const QAbstractItemModel *model = w.findChild<QTableView *>()->model();
        for (const QString &choice : QStringList{"Alpha", "Beta", "Alpha"}) {
            const int index = game->findText(choice);
            game->setCurrentIndex(index);
            emit game->activated(index);
            QTRY_COMPARE(model->headerData(0, Qt::Horizontal).toString(), choice.toLower() + " name");
        }
        QFile::remove(folder + "Alpha.json");
        QFile::remove(folder + "Beta.json");
    }

    void selectingAFolderReplacesTheViewer()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("Sounds/inner"));
        for (const QString &name : QStringList{"items.tbl", "Sounds/a.bin", "Sounds/b.bin"}) {
            QFile f(dir.filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(sample::makeSample());
        }
        MainWindow w;
        w.show();
        QVERIFY(drop(w, dir.path()));
        QTRY_COMPARE(page(w), 0);
        auto *message = qobject_cast<QLabel *>(w.findChild<QStackedWidget *>()->widget(0));
        QVERIFY(message);
        QTRY_VERIFY(message->text().contains("1 file") && message->text().contains("1 folder"));

        auto *tree = w.findChild<QTreeView *>();
        auto *fs = w.findChild<QFileSystemModel *>();
        auto *table = w.findChild<QTableView *>();
        QModelIndex file, folder;
        QTRY_VERIFY((file = fs->index(dir.filePath("items.tbl"))).isValid());
        QTRY_VERIFY((folder = fs->index(dir.filePath("Sounds"))).isValid());
        tree->setCurrentIndex(file);
        QTRY_COMPARE(page(w), 1);
        QVERIFY(table->model()->rowCount() > 0);

        tree->setCurrentIndex(folder);
        QCOMPARE(page(w), 0);
        QVERIFY(message->text().contains("Sounds"));
        QVERIFY(message->text().contains("2 files") && message->text().contains("1 folder"));
        QVERIFY(message->text().contains(".bin (2)"));
        QCOMPARE(table->model()->rowCount(), 0);

        tree->setCurrentIndex(file);
        tree->setCurrentIndex(folder);
        QTest::qWait(500);
        QCOMPARE(page(w), 0);
        QVERIFY(message->text().contains("Sounds"));
    }

    void newestFileWins()
    {
        // Choosing files quickly one after the other (e.g. with the arrow keys): whatever
        // order the background loads end in, the last file chosen is the one shown.
        QFile other(m_dir.filePath("other.bin"));
        QVERIFY(other.open(QIODevice::WriteOnly));
        other.write(QByteArray("\0\1\2 not a known format", 23));
        other.close();

        MainWindow w;
        QVERIFY(drop(w, m_dir.filePath("items.tbl")));
        QVERIFY(drop(w, other.fileName()));
        const QAbstractItemModel *model = w.findChild<QTableView *>()->model();
        QTRY_COMPARE(page(w), 1);
        QTRY_COMPARE(model->headerData(1, Qt::Horizontal).toString(), QString("text"));
        QTest::qWait(50);
        QCOMPARE(model->headerData(1, Qt::Horizontal).toString(), QString("text"));
    }

    void fileMenuOffersFileActions()
    {
        MainWindow w;
        w.show();
        QVERIFY(drop(w, m_dir.path()));
        auto *tree = w.findChild<QTreeView *>();
        auto *fs = w.findChild<QFileSystemModel *>();
        QModelIndex item;
        QTRY_VERIFY((item = fs->index(m_dir.filePath("items.tbl"))).isValid() && !tree->visualRect(item).isEmpty());

        QStringList texts, openAsTexts;
        QTimer::singleShot(0, this, [&texts, &openAsTexts] {
            if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
                for (QAction *a : menu->actions()) {
                    texts << a->text();
                    if (a->menu() && a->text() == "Open as")
                        for (QAction *b : a->menu()->actions())
                            openAsTexts << b->text();
                }
                menu->close();
            }
        });
        emit tree->customContextMenuRequested(tree->visualRect(item).center());
        QVERIFY(!texts.contains("Open"));
        QVERIFY(texts.contains("Show in file manager"));
        QVERIFY(texts.contains("Open as"));
        QVERIFY(texts.contains("Copy path"));
        QVERIFY(!openAsTexts.isEmpty());
        QVERIFY(openAsTexts.first().startsWith("Auto-detect"));
        QVERIFY(openAsTexts.contains("Other formats"));
    }

    void pathBarGoesUpAndToTypedFolders()
    {
        QVERIFY(QDir(m_dir.path()).mkpath("inner"));
        MainWindow w;
        w.show();
        QVERIFY(drop(w, m_dir.filePath("inner")));
        auto *fs = w.findChild<QFileSystemModel *>();
        QCOMPARE(fs->rootPath(), m_dir.filePath("inner"));

        QAction *up = nullptr;
        for (QAction *a : w.findChildren<QAction *>())
            if (a->text() == "Up")
                up = a;
        QVERIFY(up && up->isEnabled());
        QVERIFY(QDir(m_dir.path()).mkpath("inner/deeper"));
        auto *tree = w.findChild<QTreeView *>();
        QTRY_VERIFY(fs->index(m_dir.filePath("inner/deeper")).isValid());
        tree->setCurrentIndex(fs->index(m_dir.filePath("inner/deeper")));
        up->trigger();
        QCOMPARE(fs->rootPath(), m_dir.path());
        QCOMPARE(fs->filePath(tree->currentIndex()), m_dir.filePath("inner"));
        QVERIFY(!tree->isExpanded(fs->index(m_dir.filePath("inner"))));

        QLineEdit *pathEdit = nullptr;
        for (QLineEdit *e : w.findChildren<QLineEdit *>())
            if (e->text() == QDir::toNativeSeparators(m_dir.path()))
                pathEdit = e;
        QVERIFY(pathEdit);
        pathEdit->setText(m_dir.filePath("inner"));
        QTest::keyClick(pathEdit, Qt::Key_Return);
        QCOMPARE(fs->rootPath(), m_dir.filePath("inner"));

        pathEdit->setText(m_dir.filePath("missing"));
        QTest::keyClick(pathEdit, Qt::Key_Return);
        QCOMPARE(fs->rootPath(), m_dir.filePath("inner"));
        QCOMPARE(pathEdit->text(), QDir::toNativeSeparators(m_dir.filePath("inner")));
    }

    void modelsCanBeExported()
    {
        QFile f(m_dir.filePath("figure.mdl"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(sample::makeRichModel());
        f.close();

        MainWindow w;
        QAction *exportModel = nullptr;
        for (QAction *a : w.findChildren<QAction *>())
            if (a->text().startsWith("Model as glTF"))
                exportModel = a;
        QVERIFY(exportModel);
        QVERIFY(!exportModel->isEnabled());
        QVERIFY(drop(w, f.fileName()));
        QTRY_VERIFY(exportModel->isEnabled());

        QVERIFY(drop(w, m_dir.filePath("items.tbl")));
        QTRY_VERIFY(!exportModel->isEnabled());
    }

    void fileTreeUsesFastIcons()
    {
        MainWindow w;
        const auto *fs = w.findChild<QFileSystemModel *>();
        QVERIFY(fs);
        QVERIFY(dynamic_cast<const FastIconProvider *>(fs->iconProvider()));
        QCOMPARE(FastIconProvider().type(QFileInfo(m_dir.filePath("items.tbl"))), QString("TBL file"));
    }

    void searchFindsFilesInClosedFolders()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("Model/Deep/Inside"));
        QVERIFY(QDir(dir.path()).mkpath("Sound"));
        const QString target = dir.filePath("Model/Deep/Inside/NpcTarget.tbl");
        {
            QFile f(target);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(sample::makeSample());
        }
        MainWindow w;
        w.show();
        QVERIFY(drop(w, dir.path()));
        auto *tree = w.findChild<QTreeView *>();
        auto *fs = w.findChild<QFileSystemModel *>();
        QLineEdit *filter = nullptr;
        for (QLineEdit *edit : w.findChildren<QLineEdit *>())
            if (edit->placeholderText() == "Filter files…")
                filter = edit;
        QVERIFY(filter);
        QLabel *status = nullptr;
        filter->setText("target");
        QTRY_VERIFY_WITH_TIMEOUT(tree->selectionModel()->isSelected(fs->index(target))
                                     && !tree->visualRect(fs->index(target)).isEmpty(),
                                 5000);
        QVERIFY(tree->isExpanded(fs->index(dir.filePath("Model/Deep"))));
        for (QLabel *label : w.findChildren<QLabel *>())
            if (label->text() == "1 of 1")
                status = label;
        QVERIFY(status);

        QTest::keyClick(filter, Qt::Key_Return);
        QTRY_COMPARE(fs->filePath(tree->currentIndex()), target);

        filter->setText("nothing-like-this");
        QTRY_VERIFY(status->text() == "No matches");
    }

    void eachCommandHasOneButton()
    {
        MainWindow w;
        for (QLabel *label : w.findChildren<QLabel *>())
            QVERIFY(!label->text().contains("<a href"));
        auto *toolbar = w.findChild<QToolBar *>();
        QVERIFY(toolbar);
        QMap<QString, int> buttons;
        for (QToolButton *button : w.findChildren<QToolButton *>()) {
            const QString text = button->defaultAction() ? button->defaultAction()->text() : button->text();
            if (!text.isEmpty())
                ++buttons[text];
        }
        for (const QString &command : QStringList{"Open folder…", "Open file…", "Reload"}) {
            QCOMPARE(buttons.value(command), 1);
            bool onToolbar = false;
            for (QToolButton *button : toolbar->findChildren<QToolButton *>())
                onToolbar = onToolbar || (button->defaultAction() ? button->defaultAction()->text() : button->text()) == command;
            QVERIFY2(onToolbar, qPrintable(command));
        }
        for (const QString &moved : QStringList{"Extract…", "Convert…", "Mod"})
            QCOMPARE(buttons.value(moved), 0);

        QMap<QString, QStringList> menus;
        for (QAction *top : w.menuBar()->actions())
            if (top->menu())
                for (QAction *a : top->menu()->actions())
                    if (!a->isSeparator())
                        menus[top->text().remove(u'&')] << a->text();
        QCOMPARE(QStringList(menus.keys()).size(), 4);
        QVERIFY(menus["File"].contains("Open folder…"));
        QVERIFY(menus["File"].contains("Extract game…"));
        QVERIFY(menus["File"].contains("Quit"));
        QVERIFY(menus["Extract"].contains("Extract…"));
        QVERIFY(menus["Extract"].contains("Model as glTF (.glb)…"));
        QVERIFY(!menus.contains("Convert"));
        QVERIFY(!menus.contains("Export"));
        QVERIFY(menus["Mod"].contains("New mod…"));
        QVERIFY(menus["Help"].contains("How to extract a game…"));
        QVERIFY(menus["Help"].contains("How to make a mod…"));
        for (QAction *top : w.menuBar()->actions())
            if (top->menu())
                for (QAction *a : top->menu()->actions())
                    QVERIFY2(!a->isIconVisibleInMenu() || a->icon().isNull(), qPrintable(a->text()));
        bool toolbarIcon = false;
        for (QAction *a : toolbar->actions())
            toolbarIcon = toolbarIcon || (a->text() == "Open folder…" && !a->icon().isNull());
        QVERIFY(toolbarIcon);
    }

    void extractDialogListsGamesAndSuggestsAFolder()
    {
        QSettings().remove("switch");
        QTemporaryDir dir;
        const QString keys = switchsamples::writeFile(dir, "prod.keys", switchsamples::keysText());
        const QString game = switchsamples::writeFile(dir, "game.nsp", switchsamples::partition(switchsamples::packageFiles()));
        const QString update = switchsamples::writeFile(dir, "update.nsp", switchsamples::partition(switchsamples::packageFiles(0x81)));
        const QString junk = switchsamples::writeFile(dir, "junk.nsp", "not a game");

        GameExtractDialog dialog(nullptr, KeySearchPlaces{});
        QString error;
        QVERIFY(!dialog.setKeyFile(dir.filePath("missing.keys"), &error));
        QVERIFY2(dialog.setKeyFile(keys, &error), qPrintable(error));
        QCOMPARE(QSettings().value("switch/prodKeys").toString(), keys);
        dialog.addPackages({game, update, junk});
        QTRY_VERIFY(!dialog.isScanning());
        QCOMPARE(dialog.titleCount(), 2);
        auto *list = dialog.findChild<QTreeWidget *>();
        QVERIFY(list);
        QPushButton *extract = nullptr;
        for (QPushButton *b : dialog.findChildren<QPushButton *>())
            if (b->text() == "Extract…")
                extract = b;
        QVERIFY(extract);
        QTreeWidgetItem *gameItem = nullptr, *updateItem = nullptr;
        QCOMPARE(list->topLevelItemCount(), 1);
        gameItem = list->topLevelItem(0);
        QCOMPARE(gameItem->childCount(), 1);
        updateItem = gameItem->child(0);
        QCOMPARE(updateItem->text(3), QString("Update"));
        QVERIFY(gameItem->text(3).startsWith("Game"));
        QCOMPARE(gameItem->text(0), QString("Test Game"));
        QCOMPARE(gameItem->text(1), QString("0100AAAA00000000"));
        QCOMPARE(gameItem->text(2), QString("1.2.3 (v65536)"));
        QLabel *status = nullptr;
        for (QLabel *l : dialog.findChildren<QLabel *>())
            if (l->text().contains("title(s) found") || l->text().contains("titles found"))
                status = l;
        QVERIFY(status);
        const QString baseStatus = status->text();
        list->setCurrentItem(updateItem);
        QVERIFY(!extract->isEnabled());
        QVERIFY(!updateItem->toolTip(0).isEmpty());
        QVERIFY(status->text().contains("cannot be extracted"));
        list->setCurrentItem(gameItem);
        QVERIFY(extract->isEnabled());
        QCOMPARE(status->text(), baseStatus);
        const auto selected = dialog.selectedTitle();
        QVERIFY(selected);
        QCOMPARE(GameExtractDialog::suggestedFolderName(*selected), QString("Test Game [0100AAAA00000000]"));
        SwitchTitle odd = *selected;
        odd.name = "A/B: C?";
        QCOMPARE(GameExtractDialog::suggestedFolderName(odd), QString("A B C [0100AAAA00000000]"));
        SwitchTitle unnamed = *selected;
        unnamed.name.clear();
        unnamed.package = "/games/Some Game [Extra Pack] [0100aaaa00000000][v0].nsp";
        QCOMPARE(unnamed.folderName(), QString("Some Game [Extra Pack] [0100AAAA00000000]"));
        unnamed.package = "/games/Other [0100BBBB00000000].nsp";
        QCOMPARE(unnamed.folderName(), QString("Other [0100BBBB00000000] [0100AAAA00000000]"));

        dialog.addPackages({game});
        QTRY_VERIFY(!dialog.isScanning());
        QCOMPARE(dialog.titleCount(), 2);

        ExtractDestinationDialog destination(*selected, "/games/Test Game [0100AAAA00000000]");
        QCOMPARE(destination.destination(), QString("/games/Test Game [0100AAAA00000000]"));
        QPushButton *go = nullptr;
        for (QPushButton *b : destination.findChildren<QPushButton *>())
            if (b->text() == "Extract")
                go = b;
        QVERIFY(go);
        destination.setDestination("relative/path");
        QVERIFY(!go->isEnabled());
        destination.setDestination(dir.filePath("out"));
        QVERIFY(go->isEnabled());
        QCOMPARE(destination.destination(), dir.filePath("out"));
        QSettings().remove("switch");
    }

    void extractDialogVerifiesTheSelectedGame()
    {
        QSettings().remove("switch");
        QTemporaryDir dir;
        const QString keys = switchsamples::writeFile(dir, "prod.keys", switchsamples::keysText());
        const QByteArray package = switchsamples::partition(switchsamples::packageFiles());
        const QString game = switchsamples::writeFile(dir, "game.nsp", package);

        auto verify = [&](const QString &path) {
            GameExtractDialog dialog(nullptr, KeySearchPlaces{});
            QString error;
            if (!dialog.setKeyFile(keys, &error))
                return QStringLiteral("keys: ") + error;
            dialog.addPackages({path});
            if (!QTest::qWaitFor([&] { return !dialog.isScanning(); }, 10000))
                return QStringLiteral("scan did not finish");
            auto *list = dialog.findChild<QTreeWidget *>();
            if (!list || list->topLevelItemCount() == 0)
                return QStringLiteral("no game listed");
            list->setCurrentItem(list->topLevelItem(0));
            QPushButton *button = nullptr;
            for (QPushButton *b : dialog.findChildren<QPushButton *>())
                if (b->text() == "Verify")
                    button = b;
            if (!button || !button->isEnabled())
                return QStringLiteral("verify button missing or disabled");
            QString shown;
            QTimer poll;
            poll.setInterval(20);
            connect(&poll, &QTimer::timeout, this, [&] {
                if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                    shown = box->text();
                    box->accept();
                }
            });
            poll.start();
            button->click();
            QTest::qWaitFor([&] { return !shown.isEmpty(); }, 20000);
            poll.stop();
            return shown;
        };

        const QString intact = verify(game);
        QVERIFY2(intact.contains("intact"), qPrintable(intact));

        std::vector<std::pair<QByteArray, QByteArray>> files = switchsamples::packageFiles();
        QByteArray &program = files.front().second;
        program[program.size() - 0x300] = char(program[program.size() - 0x300] ^ 0x55);
        const QString broken = verify(switchsamples::writeFile(dir, "broken.nsp", switchsamples::partition(files)));
        QVERIFY2(broken.contains("damaged"), qPrintable(broken));
        QSettings().remove("switch");
    }

    void extractDialogReadsNupWithoutKeys()
    {
        QSettings().remove("switch");
        QTemporaryDir dir;
        switchsamples::NupPackageSpec base{0x0400AAAA00000000ull, 0x10000, 0,
                                           {{"exefs/main", "NSO0", false}, {"romfs/a.txt", "hello", true}},
                                           {{5, "Nup Game"}}};
        switchsamples::NupPackageSpec update{0x0400AAAA00000800ull, 0x20000, 1, {{"romfs/a.txt", "hello 2", true}}, {}};
        const QString game = switchsamples::writeFile(dir, "game.nup", switchsamples::nupFile({base, update}));
        GameExtractDialog dialog(nullptr, KeySearchPlaces{});
        dialog.addPackages({game});
        QTRY_VERIFY(!dialog.isScanning());
        QCOMPARE(dialog.titleCount(), 2);
        auto *list = dialog.findChild<QTreeWidget *>();
        QCOMPARE(list->topLevelItemCount(), 1);
        QCOMPARE(list->topLevelItem(0)->text(0), QString("Nup Game"));
        QCOMPARE(list->topLevelItem(0)->childCount(), 1);
        QPushButton *extract = nullptr;
        for (QPushButton *b : dialog.findChildren<QPushButton *>())
            if (b->text() == "Extract…")
                extract = b;
        QVERIFY(extract);
        list->setCurrentItem(list->topLevelItem(0));
        QVERIFY(extract->isEnabled());
        list->setCurrentItem(list->topLevelItem(0)->child(0));
        QVERIFY(extract->isEnabled());
        QSettings().remove("switch");
    }

    void extractDialogFindsKeysAutomatically()
    {
        QSettings().remove("switch");
        QTemporaryDir root;
        KeySearchPlaces places;
        places.home = root.filePath("home");
        places.dataHome = root.filePath("home/.local/share");
        places.configHome = root.filePath("home/.config");
        const QString old = switchsamples::writeFile(root, "home/.local/share/yuzu/keys/prod.keys", switchsamples::keysText(0));
        const QString game = switchsamples::writeFile(
            root, "games/new.nsp", switchsamples::partition(switchsamples::packageFiles(0x80, {}, true, 3)));

        {
            GameExtractDialog dialog(nullptr, places);
            QCOMPARE(dialog.keyFile(), QFileInfo(old).absoluteFilePath());
            QVERIFY(!dialog.keysChosenByUser());
            QLabel *keysLabel = nullptr;
            for (QLabel *l : dialog.findChildren<QLabel *>())
                if (l->text().contains("prod.keys"))
                    keysLabel = l;
            QVERIFY(keysLabel);
            QVERIFY(keysLabel->text().contains("found automatically"));
            bool change = false;
            for (QPushButton *b : dialog.findChildren<QPushButton *>())
                change = change || b->text() == "Change…";
            QVERIFY(change);

            dialog.addPackages({game});
            QTRY_VERIFY(!dialog.isScanning());
            bool told = false;
            for (QLabel *l : dialog.findChildren<QLabel *>())
                told = told || l->text().contains("master_key_03");
            QVERIFY(told);
            QCOMPARE(dialog.keyFile(), QFileInfo(old).absoluteFilePath());

            const QString good = switchsamples::writeFile(root, "games/prod.keys", switchsamples::keysText(3));
            const QString other = switchsamples::writeFile(root, "games/new2.nsp",
                                                           switchsamples::partition(switchsamples::packageFiles(0x80, {}, true, 3)));
            dialog.addPackages({other});
            QTRY_VERIFY(!dialog.isScanning());
            QCOMPARE(dialog.keyFile(), QFileInfo(good).absoluteFilePath());
            QVERIFY(!dialog.keysChosenByUser());
            QVERIFY(dialog.titleCount() >= 1);
            QVERIFY(QSettings().value("switch/prodKeys").toString().isEmpty());

            QString error;
            QVERIFY2(dialog.setKeyFile(old, &error), qPrintable(error));
            QVERIFY(dialog.keysChosenByUser());
            QTRY_VERIFY(!dialog.isScanning());
            QCOMPARE(dialog.keyFile(), QFileInfo(old).absoluteFilePath());
        }

        GameExtractDialog again(nullptr, places);
        QCOMPARE(again.keyFile(), QFileInfo(old).absoluteFilePath());
        QVERIFY(again.keysChosenByUser());
        QSettings().remove("switch");
    }

    void ignoresNonFiles()
    {
        MainWindow w;
        QMimeData mime;
        mime.setText("just some text");
        QDragEnterEvent enter(QPointF(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&w, &enter);
        QVERIFY(!enter.isAccepted());
    }

    void animationDialogTicksFoundFilesAndFilters()
    {
        auto model = std::make_shared<Model3D>();
        const std::vector<AnimationFile> found = {{"/a/chr_m_run.mdl", "chr_m_run", 3, 3},
                                                  {"/a/chr_m_walk.mdl", "chr_m_walk", 3, 4}};
        AnimationDialog dialog(found, model, "/a");
        auto paths = [&dialog] {
            std::vector<QString> out;
            for (const AnimationFile &f : dialog.selected())
                out.push_back(f.path);
            return out;
        };
        QCOMPARE(paths(), (std::vector<QString>{"/a/chr_m_run.mdl", "/a/chr_m_walk.mdl"}));

        dialog.findChild<QLineEdit *>()->setText("walk");
        for (QPushButton *button : dialog.findChildren<QPushButton *>())
            if (button->text() == "Select none")
                button->click();
        QCOMPARE(paths(), (std::vector<QString>{"/a/chr_m_run.mdl"}));
        QVERIFY(dialog.findChild<QListWidget *>()->item(0)->isHidden());
        QVERIFY(!dialog.findChild<QListWidget *>()->item(1)->isHidden());
    }

    void fileMenuExportsTheSelection()
    {
        MainWindow w;
        w.show();
        {
            QFile f(m_dir.filePath("second.tbl"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(sample::makeSample());
        }
        QVERIFY(drop(w, m_dir.path()));
        auto *tree = w.findChild<QTreeView *>();
        auto *fs = w.findChild<QFileSystemModel *>();
        QModelIndex first, second;
        QTRY_VERIFY((first = fs->index(m_dir.filePath("items.tbl"))).isValid() && !tree->visualRect(first).isEmpty());
        QTRY_VERIFY((second = fs->index(m_dir.filePath("second.tbl"))).isValid() && !tree->visualRect(second).isEmpty());
        tree->selectionModel()->select(first, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        tree->selectionModel()->select(second, QItemSelectionModel::Select | QItemSelectionModel::Rows);

        QStringList texts;
        QTimer::singleShot(0, this, [&texts] {
            if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
                for (QAction *a : menu->actions())
                    texts << a->text();
                menu->close();
            }
        });
        emit tree->customContextMenuRequested(tree->visualRect(second).center());
        QVERIFY(texts.contains("Extract 2 items…"));
        QVERIFY(!texts.contains("Convert 2 items…"));

        bool found = false;
        for (QAction *a : w.findChildren<QAction *>())
            found = found || (a->text() == "Extract…" && a->isEnabled());
        QVERIFY(found);
    }

    void batchDialogNeedsAKindAndAFolder()
    {
        BatchExportDialog dialog("3 file(s) selected", "/game");
        dialog.setOutputFolder("/out");
        auto *ok = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        for (QCheckBox *box : dialog.findChildren<QCheckBox *>())
            if (box->text().startsWith("Tables"))
                box->setChecked(true);
        QVERIFY(ok->isEnabled());
        QVERIFY(dialog.options().kinds & BatchCsv);
        QCOMPARE(dialog.options().outputFolder, QString("/out"));
        for (QCheckBox *box : dialog.findChildren<QCheckBox *>())
            if (box->text().contains("→"))
                box->setChecked(false);
        QVERIFY(ok->isEnabled());
        for (QCheckBox *box : dialog.findChildren<QCheckBox *>())
            if (box->text().startsWith("Save files that cannot") || box->text().startsWith("Also save the original"))
                box->setChecked(false);
        QVERIFY(!ok->isEnabled());
        QVERIFY(!dialog.options().keepUnconverted);
        QVERIFY(dialog.options().openArchives);
    }

    void archiveEntriesLookOpenable()
    {
        QFile f(m_dir.filePath("mixed.sarc"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(samplesarc::make({{"body.bfres", QByteArray("FRES    ") + QByteArray(64, '\0')},
                                  {"inner.sarc", samplesarc::make({{"x.txt", "x"}})},
                                  {"readme.txt", "just text"}}));
        f.close();

        MainWindow w;
        w.show();
        auto *table = w.findChild<QTableView *>();
        QVERIFY(drop(w, m_dir.filePath("mixed.sarc")));
        QTRY_COMPARE(table->model()->rowCount(), 3);

        QLabel *hint = nullptr;
        for (QLabel *label : w.findChildren<QLabel *>())
            if (label->text().startsWith("Double-click an entry"))
                hint = label;
        QVERIFY(hint);
        QTRY_VERIFY(hint->isVisible());

        auto iconOf = [table](const QString &name) {
            for (int r = 0; r < table->model()->rowCount(); ++r)
                if (table->model()->index(r, 0).data().toString() == name)
                    return table->model()->index(r, 0).data(Qt::DecorationRole).value<QIcon>().cacheKey();
            return qint64(-1);
        };
        QCOMPARE(iconOf("body.bfres"), contentIcon(ContentCategory::Model).cacheKey());
        QCOMPARE(iconOf("inner.sarc"), contentIcon(ContentCategory::Archive).cacheKey());
        QCOMPARE(iconOf("readme.txt"), contentIcon(ContentCategory::Other).cacheKey());

        const QPoint at = table->visualRect(table->model()->index(1, 0)).center();
        QMouseEvent move(QEvent::MouseMove, at, table->viewport()->mapToGlobal(at), Qt::NoButton, Qt::NoButton,
                         Qt::NoModifier);
        QCoreApplication::sendEvent(table->viewport(), &move);
        QCOMPARE(table->viewport()->cursor().shape(), Qt::PointingHandCursor);
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(table->viewport(), &leave);
        QVERIFY(table->viewport()->cursor().shape() != Qt::PointingHandCursor);

        for (int r = 0; r < table->model()->rowCount(); ++r)
            if (table->model()->index(r, 0).data().toString() == "readme.txt")
                emit table->activated(table->model()->index(r, 0));
        QTRY_VERIFY(!hint->isVisible());
        QCoreApplication::sendEvent(table->viewport(), &move);
        QVERIFY(table->viewport()->cursor().shape() != Qt::PointingHandCursor);
        QVERIFY(!table->model()->index(0, 0).data(Qt::DecorationRole).isValid());
    }

    void formatAppliesToTheOpenArchiveEntry()
    {
        QFile f(m_dir.filePath("formats.sarc"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(samplesarc::make({{"note.txt", "hello there"}, {"other.txt", "x"}}));
        f.close();
        MainWindow w;
        w.show();
        auto *table = w.findChild<QTableView *>();
        QComboBox *format = nullptr, *encoding = nullptr;
        for (QComboBox *box : w.findChildren<QComboBox *>()) {
            if (box->toolTip().startsWith("Read files as"))
                format = box;
            if (box->toolTip().startsWith("How text"))
                encoding = box;
        }
        QVERIFY(format && encoding);
        QVERIFY(!encoding->isEnabled());
        auto locationShows = [&w](const QString &text) {
            for (QLabel *label : w.findChildren<QLabel *>())
                if (label->isVisible() && label->text().contains(text) && label->text().contains("&rsaquo;"))
                    return true;
            return false;
        };
        QVERIFY(drop(w, m_dir.filePath("formats.sarc")));
        QTRY_COMPARE(table->model()->rowCount(), 2);
        QVERIFY(!encoding->isEnabled());
        for (int r = 0; r < 2; ++r)
            if (table->model()->index(r, 0).data().toString() == "note.txt")
                emit table->activated(table->model()->index(r, 0));
        QTRY_VERIFY(locationShows("note.txt"));
        QTRY_VERIFY(encoding->isEnabled());

        format->setCurrentIndex(format->findText("Hex"));
        QTRY_VERIFY(table->model()->rowCount() > 0 && table->model()->index(0, 0).data().toString().startsWith("68 65 6C"));
        QVERIFY(locationShows("note.txt"));
        format->setCurrentIndex(0);
        QTRY_VERIFY(table->model()->index(0, 0).data().toString() == "hello there");
        QVERIFY(locationShows("note.txt"));

        QVERIFY2(format->count() < 10, qPrintable(QString::number(format->count())));
        QVERIFY(format->itemText(0).startsWith("Auto-detect"));
        QVERIFY(format->findText("Text") > 0);
        QVERIFY(format->findText("Hex") > 0);
        QVERIFY(format->findText("RAGE model") < 0);
        const int other = format->findText("Other formats…");
        QVERIFY(other > 0);

        bool picked = false;
        QTimer poll;
        poll.setInterval(20);
        connect(&poll, &QTimer::timeout, this, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu || picked)
                return;
            for (QAction *family : menu->actions())
                if (family->text() == "Rockstar (RAGE)" && family->menu())
                    for (QAction *a : family->menu()->actions())
                        if (a->text() == "RAGE model") {
                            picked = true;
                            a->trigger();
                        }
            menu->close();
        });
        poll.start();
        format->setCurrentIndex(other);
        QTRY_VERIFY(picked);
        poll.stop();
        QTRY_COMPARE(format->currentText(), QString("RAGE model"));
        QTRY_VERIFY(locationShows("note.txt"));
        format->setCurrentIndex(0);
        QTRY_VERIFY(table->model()->index(0, 0).data().toString() == "hello there");
    }

    void archivesCanBeBrowsed()
    {
        auto write = [this](const QString &name, const QByteArray &data) {
            QFile f(m_dir.filePath(name));
            return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
        };
        QVERIFY(write("pack.sarc", samplesarc::make({{"a/first.txt", "hello"}, {"second.txt", "world"}})));
        QVERIFY(write("single.sarc", samplesarc::make({{"only.txt", "alone"}})));

        MainWindow w;
        w.show();
        auto *table = w.findChild<QTableView *>();
        QAction *back = nullptr;
        for (QAction *a : w.findChildren<QAction *>())
            if (a->text() == "Back")
                back = a;
        QVERIFY(back);
        auto locationShows = [&w](const QString &text) {
            for (QLabel *label : w.findChildren<QLabel *>())
                if (label->isVisible() && label->text().contains(text) && label->text().contains("&rsaquo;"))
                    return true;
            return false;
        };

        QVERIFY(drop(w, m_dir.filePath("pack.sarc")));
        QTRY_COMPARE(table->model()->rowCount(), 2);
        QVERIFY(!back->isEnabled());

        emit table->activated(table->model()->index(0, 0));
        QTRY_VERIFY(locationShows("a/first.txt"));
        QVERIFY(back->isEnabled());

        back->trigger();
        QTRY_VERIFY(!back->isEnabled());
        QTRY_COMPARE(table->model()->rowCount(), 2);

        QVERIFY(drop(w, m_dir.filePath("single.sarc")));
        QTRY_VERIFY(locationShows("only.txt"));
        back->trigger();
        QTRY_VERIFY(!back->isEnabled());
        QTRY_COMPARE(table->model()->rowCount(), 1);
        QTest::qWait(100);
        QVERIFY(!back->isEnabled());

        QVERIFY(write("outer.sarc", samplesarc::make({{"inner.sarc", samplesarc::make({{"deep.txt", "deep"}})},
                                                     {"other.txt", "other"}})));
        QVERIFY(drop(w, m_dir.filePath("outer.sarc")));
        QTRY_COMPARE(table->model()->rowCount(), 2);
        for (int row = 0; row < 2; ++row)
            if (table->model()->index(row, 0).data().toString() == "inner.sarc")
                emit table->activated(table->model()->index(row, 0));
        QTRY_VERIFY(locationShows("deep.txt"));
        QVERIFY(back->isEnabled());
        back->trigger();
        QTRY_VERIFY(!back->isEnabled());
        QTRY_COMPARE(table->model()->rowCount(), 2);
    }

    void batchExportSummaryDoesNotBlockTheWindow()
    {
        QTemporaryDir out;
        MainWindow w;
        w.show();
        QVERIFY(drop(w, m_dir.path()));
        auto *tree = w.findChild<QTreeView *>();
        auto *fs = w.findChild<QFileSystemModel *>();
        QModelIndex item;
        QTRY_VERIFY((item = fs->index(m_dir.filePath("items.tbl"))).isValid() && !tree->visualRect(item).isEmpty());
        tree->selectionModel()->select(item, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);

        QAction *exportSelected = nullptr;
        for (QAction *a : w.findChildren<QAction *>())
            if (a->text() == "Extract…")
                exportSelected = a;
        QVERIFY(exportSelected);

        bool progressShownWithSummary = false, summarySeen = false;
        QString summary;
        QElapsedTimer summaryTime;
        QTimer poll;
        poll.setInterval(20);
        connect(&poll, &QTimer::timeout, this, [&] {
            QWidget *modal = QApplication::activeModalWidget();
            if (auto *dialog = qobject_cast<BatchExportDialog *>(modal)) {
                dialog->setOutputFolder(out.path());
                for (QCheckBox *box : dialog->findChildren<QCheckBox *>())
                    if (box->text().startsWith("Tables") || box->text().startsWith("Save files that cannot"))
                        box->setChecked(false);
                dialog->accept();
            } else if (auto *progress = qobject_cast<QProgressDialog *>(modal); progress && summarySeen) {
                progressShownWithSummary = true;
                progress->hide();
            } else if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                if (!summarySeen) {
                    summarySeen = true;
                    summaryTime.start();
                }
                progressShownWithSummary = progressShownWithSummary || !w.findChildren<QProgressDialog *>().isEmpty();
                if (summaryTime.elapsed() > 600) {
                    summary = box->text();
                    box->accept();
                }
            }
        });
        QEventLoop loop;
        connect(&poll, &QTimer::timeout, &loop, [&] {
            if (!summary.isEmpty())
                loop.quit();
        });
        poll.start();
        QTimer::singleShot(0, &loop, [&] { exportSelected->trigger(); });
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        poll.stop();
        QVERIFY(!summary.isEmpty());
        QVERIFY(!progressShownWithSummary);
        QTRY_VERIFY(QApplication::activeModalWidget() == nullptr);
        QVERIFY(w.findChildren<QProgressDialog *>().isEmpty());
        QVERIFY2(summary.startsWith("Nothing was extracted"), qPrintable(summary));
    }

    void batchExportOfOneFileLandsInTheChosenFolder()
    {
        QTemporaryDir out;
        MainWindow w;
        w.show();
        QVERIFY(drop(w, QFileInfo(m_dir.path()).absolutePath()));
        auto *tree = w.findChild<QTreeView *>();
        auto *fs = w.findChild<QFileSystemModel *>();
        QModelIndex folder, item;
        QTRY_VERIFY((folder = fs->index(m_dir.path())).isValid());
        tree->expand(folder);
        QTRY_VERIFY((item = fs->index(m_dir.filePath("items.tbl"))).isValid() && !tree->visualRect(item).isEmpty());
        tree->selectionModel()->select(item, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);

        QAction *exportSelected = nullptr;
        for (QAction *a : w.findChildren<QAction *>())
            if (a->text() == "Extract…")
                exportSelected = a;
        QVERIFY(exportSelected);

        QString summary;
        QTimer poll;
        poll.setInterval(20);
        QEventLoop loop;
        connect(&poll, &QTimer::timeout, &loop, [&] {
            QWidget *modal = QApplication::activeModalWidget();
            if (auto *dialog = qobject_cast<BatchExportDialog *>(modal)) {
                dialog->setOutputFolder(out.path());
                for (QCheckBox *box : dialog->findChildren<QCheckBox *>())
                    if (box->text().startsWith("Tables"))
                        box->setChecked(true);
                dialog->accept();
            } else if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                summary = box->text();
                box->accept();
                loop.quit();
            }
        });
        poll.start();
        QTimer::singleShot(0, &loop, [&] { exportSelected->trigger(); });
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        poll.stop();

        const QStringList csv = QDir(out.path()).entryList({"items*.csv"}, QDir::Files);
        QVERIFY2(!csv.isEmpty(), qPrintable(QDir(out.path()).entryList().join(", ")));
        QVERIFY(summary.contains("Saved in " + QDir::toNativeSeparators(out.path())));
    }

    void texturesCanBeReplacedIntoAModFolder()
    {
        const QByteArray xtx = makeXtx();

        QTemporaryDir game, mod;
        QVERIFY(QDir(game.path()).mkpath("romfs/asset/image"));
        const QString texture = game.filePath("romfs/asset/image/icons.xtx");
        {
            QFile f(texture);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(xtx);
        }
        QImage picture(8, 8, QImage::Format_RGBA8888);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                picture.setPixelColor(x, y, QColor(x * 30, y * 30, 100, 255));
        const QString png = game.filePath("new.png");
        QVERIFY(picture.save(png));

        MainWindow w;
        w.setActiveMod(mod.filePath("romfs"));
        w.show();
        QVERIFY(drop(w, texture));
        QAction *replace = nullptr;
        for (QAction *a : w.findChildren<QAction *>())
            if (a->text() == "Replace texture with PNG…")
                replace = a;
        QVERIFY(replace);
        QTRY_VERIFY(replace->isEnabled());

        QString summary;
        QTimer poll;
        poll.setInterval(20);
        QEventLoop loop;
        connect(&poll, &QTimer::timeout, &loop, [&] {
            QWidget *modal = QApplication::activeModalWidget();
            if (auto *dialog = qobject_cast<QFileDialog *>(modal)) {
                dialog->selectFile(png);
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            } else if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                summary = box->text();
                box->accept();
                loop.quit();
            }
        });
        poll.start();
        QTimer::singleShot(0, &loop, [&] { replace->trigger(); });
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        poll.stop();

        QVERIFY2(summary.startsWith("Saved asset/image/icons.xtx"), qPrintable(summary));
        QFile saved(mod.filePath("romfs/asset/image/icons.xtx"));
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QString error;
        const auto doc = XtxReader::fromBytes(saved.readAll(), &error);
        QVERIFY2(doc, qPrintable(error));
        const auto image = doc->tables.front()->image();
        QVERIFY(image);
        QCOMPARE(image->rgba, QByteArray(reinterpret_cast<const char *>(picture.constBits()), 8 * 8 * 4));
        QSettings().remove("mods");
    }

    void texturesInsideArchivesCanBeReplaced()
    {
        QString error;
        const QByteArray sarc = *writeSarc({{"other.bin", "keep me", 0x10}, {"timg/icons.xtx", makeXtx(), 0x200},
                                            {"timg/second.xtx", makeXtx(), 0x200}},
                                           {}, &error);
        QTemporaryDir game, mod;
        QVERIFY(QDir(game.path()).mkpath("romfs/Layout"));
        const QString packPath = game.filePath("romfs/Layout/pack.sarc.zs");
        {
            QFile f(packPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(*compressZstd(sarc, 3, true, &error));
        }
        auto picture = [&](int seed) {
            QImage p(8, 8, QImage::Format_RGBA8888);
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x)
                    p.setPixelColor(x, y, QColor(x * 30, y * 30, seed, 255));
            const QString path = game.filePath(QString("pic%1.png").arg(seed));
            p.save(path);
            return p;
        };
        const QImage first = picture(40), second = picture(200);

        MainWindow w;
        w.setActiveMod(mod.filePath("romfs"));
        w.show();
        auto *table = w.findChild<QTableView *>();
        QAction *replace = nullptr, *back = nullptr;
        for (QAction *a : w.findChildren<QAction *>()) {
            if (a->text() == "Replace texture with PNG…")
                replace = a;
            else if (a->text() == "Back")
                back = a;
        }
        QVERIFY(replace && back);
        auto openEntry = [&](const QString &name) {
            QTRY_COMPARE(table->model()->rowCount(), 3);
            for (int row = 0; row < 3; ++row)
                if (table->model()->index(row, 0).data().toString() == name)
                    emit table->activated(table->model()->index(row, 0));
        };
        auto run = [&](const QString &png, const QString &choice, QString *summary, QString *detail, QString *asked) {
            QTimer poll;
            poll.setInterval(20);
            QEventLoop loop;
            connect(&poll, &QTimer::timeout, &loop, [&] {
                QWidget *modal = QApplication::activeModalWidget();
                if (auto *dialog = qobject_cast<QFileDialog *>(modal)) {
                    dialog->selectFile(png);
                    QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                } else if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                    if (box->text().contains("already has")) {
                        *asked = box->text();
                        for (QAbstractButton *b : box->buttons())
                            if (b->text() == choice)
                                b->click();
                        return;
                    }
                    *summary = box->text();
                    *detail = box->informativeText();
                    box->accept();
                    loop.quit();
                }
            });
            poll.start();
            QTimer::singleShot(0, &loop, [&] { replace->trigger(); });
            QTimer::singleShot(15000, &loop, &QEventLoop::quit);
            loop.exec();
        };

        QVERIFY(drop(w, packPath));
        openEntry("timg/icons.xtx");
        QTRY_VERIFY(replace->isEnabled());
        QString summary, detail, asked;
        run(game.filePath("pic40.png"), {}, &summary, &detail, &asked);
        QVERIFY2(summary.startsWith("Saved Layout/pack.sarc.zs"), qPrintable(summary));
        QVERIFY2(detail.contains("Rebuilt") && detail.contains("zstd") && detail.contains("kept their unpacked size"),
                 qPrintable(detail));
        QVERIFY(asked.isEmpty());

        back->trigger();
        openEntry("timg/second.xtx");
        QTRY_VERIFY(replace->isEnabled());
        summary.clear();
        run(game.filePath("pic200.png"), "Keep earlier changes", &summary, &detail, &asked);
        QVERIFY2(asked.contains("already has a changed Layout/pack.sarc.zs"), qPrintable(asked));
        QVERIFY2(summary.startsWith("Saved Layout/pack.sarc.zs"), qPrintable(summary));

        QFile saved(mod.filePath("romfs/Layout/pack.sarc.zs"));
        QVERIFY(saved.open(QIODevice::ReadOnly));
        std::vector<CompressionLayer> layers;
        const auto plain = unpackAll(saved.readAll(), &layers, &error);
        QVERIFY2(plain, qPrintable(error));
        QCOMPARE(layers.size(), size_t(1));
        QVERIFY(layers[0].zstdChecksum);
        const auto contents = readSarc(*plain, &error);
        QVERIFY2(contents, qPrintable(error));
        auto pixels = [&](const QString &name) {
            for (const SarcEntry &e : contents->entries)
                if (e.name == name)
                    if (auto doc = XtxReader::fromBytes(e.data, &error))
                        if (auto image = doc->tables.front()->image())
                            return image->rgba;
            return QByteArray();
        };
        auto bytes = [](const QImage &p) { return QByteArray(reinterpret_cast<const char *>(p.constBits()), 8 * 8 * 4); };
        QCOMPARE(pixels("timg/icons.xtx"), bytes(first));
        QCOMPARE(pixels("timg/second.xtx"), bytes(second));
        for (const SarcEntry &e : contents->entries)
            if (e.name == "other.bin")
                QCOMPARE(e.data, QByteArray("keep me"));
        QFile untouched(packPath);
        QVERIFY(untouched.open(QIODevice::ReadOnly));
        QCOMPARE(unpackAll(untouched.readAll(), nullptr, &error).value_or(QByteArray()), sarc);
        QSettings().remove("mods");
    }

    void archiveFilesAreExtractedInTheBackground()
    {
        QFile f(m_dir.filePath("extract.sarc"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(samplesarc::make({{"a/first.txt", "hello"}, {"second.bin", "bytes"}, {"third.txt", "bye"}}));
        f.close();
        QTemporaryDir out;

        MainWindow w;
        w.show();
        QVERIFY(drop(w, f.fileName()));
        QAction *extract = nullptr;
        for (QAction *a : w.findChildren<QAction *>())
            if (a->text() == "Extract…")
                extract = a;
        QVERIFY(extract);
        QTRY_VERIFY(extract->isEnabled());
        auto *table = w.findChild<QTableView *>();
        QTRY_COMPARE(table->model()->rowCount(), 3);

        QTimer poll;
        poll.setInterval(20);
        QEventLoop loop;
        connect(&poll, &QTimer::timeout, &loop, [&] {
            QWidget *modal = QApplication::activeModalWidget();
            if (auto *dialog = qobject_cast<BatchExportDialog *>(modal)) {
                dialog->setOutputFolder(out.path());
                for (QCheckBox *box : dialog->findChildren<QCheckBox *>())
                    if (box->text().startsWith("Save files that cannot") || box->text().startsWith("Open archives"))
                        box->setChecked(true);
                dialog->accept();
            } else if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                box->accept();
                poll.stop();
                loop.quit();
            }
        });
        poll.start();
        QTimer::singleShot(0, &loop, [&] { extract->trigger(); });
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();

        QLabel *info = nullptr;
        for (QLabel *label : w.statusBar()->findChildren<QLabel *>())
            info = label;
        QVERIFY(info);
        QTRY_VERIFY2(info->text().startsWith("Extracted 3 file(s)"), qPrintable(info->text()));
        QTRY_VERIFY(extract->isEnabled());
        QVERIFY2(info->text().contains(QDir::toNativeSeparators(out.path())), qPrintable(info->text()));
        QFile first(QDir(out.path()).filePath("a/first.txt"));
        QVERIFY(first.open(QIODevice::ReadOnly));
        QCOMPARE(first.readAll(), QByteArray("hello"));
        QVERIFY(QFileInfo::exists(QDir(out.path()).filePath("second.bin")));
        QVERIFY(QFileInfo::exists(QDir(out.path()).filePath("third.txt")));
    }

    void encryptedArchivesAskForTheGamesKeyOnce()
    {
        QSettings().remove("rpf6Keys");
        Rpf6Reader::setKeys({});
        QTemporaryDir game;
        QVERIFY(QDir(game.path()).mkpath("romfs"));
        QVERIFY(QDir(game.path()).mkpath("romfs/exefs"));
        const samplerpf6::Sample sample = samplerpf6::makeArchive();
        QFile archive(game.filePath("romfs/content.rpf"));
        QVERIFY(archive.open(QIODevice::WriteOnly));
        archive.write(sample.file);
        archive.close();
        QByteArray data(0x2000, '\0');
        data.replace(0x400, 32, samplerpf6::testKey());
        QFile exe(game.filePath("romfs/exefs/main"));
        QVERIFY(exe.open(QIODevice::WriteOnly));
        exe.write(samplerpf6::nso(QByteArray(0x100, '\0'), QByteArray(0x100, '\0'), data));
        exe.close();

        MainWindow w;
        w.show();
        QVERIFY(drop(w, archive.fileName()));
        QLabel *message = nullptr;
        QTRY_VERIFY((message = [&w]() -> QLabel * {
            for (QLabel *l : w.findChildren<QLabel *>())
                if (l->text().contains("nxgv:rpf6-key"))
                    return l;
            return nullptr;
        }()));

        QString chosenFolder;
        QTimer poll;
        poll.setInterval(20);
        QEventLoop loop;
        connect(&poll, &QTimer::timeout, &loop, [&] {
            if (auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                chosenFolder = dialog->directory().absolutePath();
                dialog->selectFile(exe.fileName());
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                poll.stop();
                loop.quit();
            }
        });
        poll.start();
        QTimer::singleShot(0, &loop, [&] { emit message->linkActivated("nxgv:rpf6-key"); });
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        QCOMPARE(chosenFolder, QFileInfo(exe.fileName()).absolutePath());

        QTableView *table = w.findChild<QTableView *>();
        QTRY_COMPARE(table->model()->rowCount(), 6);
        QCOMPARE(QSettings().value("rpf6Keys").toStringList(), QStringList{QString::fromLatin1(samplerpf6::testKey().toHex())});
        QVERIFY(!Rpf6Reader::needsKey(archive.fileName()));
        QSettings().remove("rpf6Keys");
        Rpf6Reader::setKeys({});
    }

    void switchGamesCanBeBrowsed()
    {
        QSettings().remove("switch");
        QTemporaryDir dir;
        const QString keys = switchsamples::writeFile(dir, "keys/prod.keys", switchsamples::keysText());
        const QString game = switchsamples::writeFile(dir, "game.nsp", switchsamples::partition(switchsamples::packageFiles()));

        MainWindow w;
        w.show();
        SwitchPackageReader::setKeyFile(dir.filePath("keys/missing.keys"));
        QVERIFY(drop(w, game));
        QLabel *message = nullptr;
        QTRY_VERIFY((message = [&w]() -> QLabel * {
            for (QLabel *l : w.findChildren<QLabel *>())
                if (l->text().contains("nxgv:switch-keys"))
                    return l;
            return nullptr;
        }()));

        QTimer poll;
        poll.setInterval(20);
        QEventLoop loop;
        connect(&poll, &QTimer::timeout, &loop, [&] {
            if (auto *dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
                auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
                if (!name)
                    return;
                name->setText(keys);
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                poll.stop();
                loop.quit();
            }
        });
        poll.start();
        QTimer::singleShot(0, &loop, [&] { emit message->linkActivated("nxgv:switch-keys"); });
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();

        auto *table = w.findChild<QTableView *>();
        QTRY_COMPARE(table->model()->rowCount(), 4);
        QCOMPARE(QSettings().value("switch/prodKeys").toString(), keys);
        QCOMPARE(SwitchPackageReader::keyFile(), keys);
        int textRow = -1;
        for (int r = 0; r < table->model()->rowCount(); ++r)
            if (table->model()->index(r, 0).data().toString() == "Test Game [0100AAAA00000000]/romfs/a.txt")
                textRow = r;
        QVERIFY(textRow >= 0);
        emit table->activated(table->model()->index(textRow, 0));
        QTRY_VERIFY([&] {
            for (int r = 0; r < table->model()->rowCount(); ++r)
                for (int c = 0; c < table->model()->columnCount(); ++c)
                    if (table->model()->index(r, c).data().toString().contains("hello"))
                        return true;
            return false;
        }());
        QSettings().remove("switch");
        SwitchPackageReader::setKeyFile({});
    }

    void bntxTexturesInsideArchivesCanBeReplaced()
    {
        auto picture = [](int seed) {
            QImage p(8, 8, QImage::Format_RGBA8888);
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x)
                    p.setPixelColor(x, y, QColor(x * 30, y * 30, seed, 255));
            return p;
        };
        auto texture = [](const QByteArray &name, const QImage &p) {
            samplebntx::Texture t;
            t.name = name;
            t.linear = QByteArray(reinterpret_cast<const char *>(p.constBits()), 8 * 8 * 4);
            return t;
        };
        const QByteArray bntx = samplebntx::make({texture("First", picture(10)), texture("Second", picture(20))});
        QString error;
        const QByteArray sarc = *writeSarc({{"timg/__Combined.bntx", bntx, 0x1000}, {"blyt/main.bflyt", "layout"}}, {}, &error);
        QTemporaryDir game, mod;
        QVERIFY(QDir(game.path()).mkpath("romfs/Layout"));
        const QString packPath = game.filePath("romfs/Layout/Menu.Nin_NX_NVN.zs");
        {
            QFile f(packPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(*compressZstd(sarc, 3, false, &error));
        }
        const QImage changed = picture(250);
        const QString png = game.filePath("second.png");
        QVERIFY(changed.save(png));

        MainWindow w;
        w.setActiveMod(mod.filePath("romfs"));
        w.show();
        auto *table = w.findChild<QTableView *>();
        QAction *replace = nullptr;
        for (QAction *a : w.findChildren<QAction *>())
            if (a->text() == "Replace texture with PNG…")
                replace = a;
        QVERIFY(replace);
        QVERIFY(drop(w, packPath));
        QTRY_COMPARE(table->model()->rowCount(), 2);
        for (int row = 0; row < 2; ++row)
            if (table->model()->index(row, 0).data().toString() == "timg/__Combined.bntx")
                emit table->activated(table->model()->index(row, 0));
        QComboBox *textures = nullptr;
        QTRY_VERIFY([&] {
            for (QComboBox *box : w.findChildren<QComboBox *>())
                if (box->findText("Second", Qt::MatchStartsWith) >= 0)
                    textures = box;
            return textures != nullptr;
        }());
        textures->setCurrentIndex(textures->findText("Second", Qt::MatchStartsWith));
        QTRY_VERIFY(replace->isEnabled());

        QString summary, detail, title;
        QTimer poll;
        poll.setInterval(20);
        QEventLoop loop;
        connect(&poll, &QTimer::timeout, &loop, [&] {
            QWidget *modal = QApplication::activeModalWidget();
            if (auto *dialog = qobject_cast<QFileDialog *>(modal)) {
                title = dialog->windowTitle();
                dialog->selectFile(png);
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            } else if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                summary = box->text();
                detail = box->informativeText();
                box->accept();
                loop.quit();
            }
        });
        poll.start();
        QTimer::singleShot(0, &loop, [&] { replace->trigger(); });
        QTimer::singleShot(15000, &loop, &QEventLoop::quit);
        loop.exec();
        poll.stop();

        QCOMPARE(title, QString("Replace Second with"));
        QVERIFY2(summary.startsWith("Saved Layout/Menu.Nin_NX_NVN.zs"), qPrintable(summary + detail));
        QVERIFY2(detail.contains("to hold Second"), qPrintable(detail));
        QFile saved(mod.filePath("romfs/Layout/Menu.Nin_NX_NVN.zs"));
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const auto contents = readSarc(*unpackAll(saved.readAll(), nullptr, &error), &error);
        QVERIFY2(contents, qPrintable(error));
        QByteArray newBntx;
        for (const SarcEntry &e : contents->entries)
            if (e.name == "timg/__Combined.bntx")
                newBntx = e.data;
        QCOMPARE(newBntx.size(), bntx.size());
        const auto doc = FormatRegistry().openBytes(newBntx, "a.bntx", nullptr, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(doc->tables[0]->image()->rgba, QByteArray(reinterpret_cast<const char *>(picture(10).constBits()), 256));
        QCOMPARE(doc->tables[1]->image()->rgba, QByteArray(reinterpret_cast<const char *>(changed.constBits()), 256));
        QSettings().remove("mods");
    }

    static QImage seededPicture(int seed)
    {
        QImage p(8, 8, QImage::Format_RGBA8888);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                p.setPixelColor(x, y, QColor(x * 30, y * 30, seed, 255));
        return p;
    }

    static QAction *replaceAction(MainWindow &w)
    {
        for (QAction *a : w.findChildren<QAction *>())
            if (a->text() == "Replace texture with PNG…")
                return a;
        return nullptr;
    }

    static QStringList replaceWith(QAction *replace, const QString &png, const QString &choice)
    {
        QStringList seen;
        QTimer poll;
        poll.setInterval(20);
        QEventLoop loop;
        connect(&poll, &QTimer::timeout, &loop, [&] {
            QWidget *modal = QApplication::activeModalWidget();
            if (auto *dialog = qobject_cast<QFileDialog *>(modal)) {
                dialog->selectFile(png);
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            } else if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                if (qobject_cast<QFileDialog *>(box->parentWidget())) {
                    box->accept();
                    return;
                }
                seen << box->text();
                for (QAbstractButton *b : box->buttons())
                    if (!choice.isEmpty() && b->text() == choice) {
                        b->click();
                        return;
                    }
                if (box->text().startsWith("Saved"))
                    box->accept();
                else
                    box->reject();
                loop.quit();
            }
        });
        poll.start();
        QTimer::singleShot(0, &loop, [&] { replace->trigger(); });
        QTimer::singleShot(15000, &loop, &QEventLoop::quit);
        loop.exec();
        return seen;
    }

    void standaloneMultiTextureFilesKeepEarlierChanges()
    {
        auto texture = [](const QByteArray &name, const QImage &p) {
            samplebntx::Texture t;
            t.name = name;
            t.linear = QByteArray(reinterpret_cast<const char *>(p.constBits()), 8 * 8 * 4);
            return t;
        };
        QTemporaryDir game, mod;
        QVERIFY(QDir(game.path()).mkpath("romfs/ui"));
        const QString path = game.filePath("romfs/ui/icons.bntx");
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(samplebntx::make({texture("First", seededPicture(10)), texture("Second", seededPicture(20))}));
        }
        const QImage first = seededPicture(150), second = seededPicture(250);
        QVERIFY(first.save(game.filePath("first.png")) && second.save(game.filePath("second.png")));

        MainWindow w;
        w.setActiveMod(mod.filePath("romfs"));
        w.show();
        QAction *replace = replaceAction(w);
        QVERIFY(replace);
        QVERIFY(drop(w, path));
        QComboBox *textures = nullptr;
        QTRY_VERIFY([&] {
            for (QComboBox *box : w.findChildren<QComboBox *>())
                if (box->findText("Second", Qt::MatchStartsWith) >= 0)
                    textures = box;
            return textures != nullptr;
        }());
        textures->setCurrentIndex(textures->findText("First", Qt::MatchStartsWith));
        QTRY_VERIFY(replace->isEnabled());
        QStringList seen = replaceWith(replace, game.filePath("first.png"), {});
        QVERIFY2(seen.size() == 1 && seen[0].startsWith("Saved ui/icons.bntx"), qPrintable(seen.join(" | ")));

        textures->setCurrentIndex(textures->findText("Second", Qt::MatchStartsWith));
        QTRY_VERIFY(replace->isEnabled());
        seen = replaceWith(replace, game.filePath("second.png"), "Keep earlier changes");
        QVERIFY2(seen.size() == 2 && seen[0].contains("already has a changed ui/icons.bntx"), qPrintable(seen.join(" | ")));

        QFile saved(mod.filePath("romfs/ui/icons.bntx"));
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QString error;
        const auto doc = FormatRegistry().openBytes(saved.readAll(), "icons.bntx", nullptr, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(doc->tables[0]->image()->rgba, QByteArray(reinterpret_cast<const char *>(first.constBits()), 256));
        QCOMPARE(doc->tables[1]->image()->rgba, QByteArray(reinterpret_cast<const char *>(second.constBits()), 256));
        QSettings().remove("mods");
    }

    void replacingWarnsWhenTheModIsForAnotherGame()
    {
        QTemporaryDir emu;
        QVERIFY(QDir(emu.path()).mkpath("dump/0100AAAAAAAAA000/romfs"));
        const QString texture = emu.filePath("dump/0100AAAAAAAAA000/romfs/icons.xtx");
        {
            QFile f(texture);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(makeXtx());
        }
        QVERIFY(seededPicture(90).save(emu.filePath("new.png")));
        const QString other = emu.filePath("load/0100BBBBBBBBB000/Other/romfs");

        MainWindow w;
        w.setActiveMod(other);
        w.show();
        QAction *replace = replaceAction(w);
        QVERIFY(drop(w, texture));
        QTRY_VERIFY(replace->isEnabled());
        QStringList seen = replaceWith(replace, emu.filePath("new.png"), {});
        QVERIFY2(seen.size() == 1 && seen[0].contains("from game 0100AAAAAAAAA000")
                     && seen[0].contains("mod folder is for game 0100BBBBBBBBB000"),
                 qPrintable(seen.join(" | ")));
        QVERIFY(!QFileInfo::exists(other + "/icons.xtx"));

        seen = replaceWith(replace, emu.filePath("new.png"), "Save there anyway");
        QVERIFY2(seen.size() == 2 && seen[1].startsWith("Saved icons.xtx"), qPrintable(seen.join(" | ")));
        QVERIFY(QFileInfo::exists(other + "/icons.xtx"));
        QSettings().remove("mods");
    }

    void theGameFolderIsNeverUsedAsTheModFolder()
    {
        QTemporaryDir game;
        QVERIFY(QDir(game.path()).mkpath("romfs/ui"));
        const QString texture = game.filePath("romfs/ui/icons.xtx");
        const QByteArray original = makeXtx();
        {
            QFile f(texture);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(original);
        }
        QVERIFY(seededPicture(90).save(game.filePath("new.png")));

        MainWindow w;
        w.setActiveMod(game.filePath("romfs"));
        w.show();
        QAction *replace = replaceAction(w);
        QVERIFY(drop(w, texture));
        QTRY_VERIFY(replace->isEnabled());
        const QStringList seen = replaceWith(replace, game.filePath("new.png"), {});
        QVERIFY2(seen.size() == 1 && seen[0].contains("overwrite the game's own file"), qPrintable(seen.join(" | ")));
        QFile f(texture);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), original);
        QSettings().remove("mods");
    }

    void anyFileCanBeReplacedIntoAMod()
    {
        QSettings().remove("mods");
        QTemporaryDir emu;
        const QString romfs = emu.filePath("dump/0100ABCDEF012000/romfs");
        const QString plain = romfs + "/data/plain.bin";
        const QString packed = romfs + "/data/text.bin.zs";
        const QString sarc = romfs + "/Pack/archive.sarc";
        QVERIFY(QDir().mkpath(romfs + "/data") && QDir().mkpath(romfs + "/Pack"));
        auto put = [](const QString &path, const QByteArray &data) {
            QFile f(path);
            return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
        };
        QVERIFY(put(plain, "OLD"));
        CompressionLayer zstd;
        zstd.kind = Compression::Zstd;
        QVERIFY(put(packed, compressLike("old text", zstd, nullptr).value()));
        QVERIFY(put(sarc, samplesarc::make({{"inner.txt", "inner old"}, {"keep.txt", "keep"}})));
        const QString pickedPlain = emu.filePath("picked.bin"), pickedText = emu.filePath("picked.txt");
        QVERIFY(put(pickedPlain, "NEW BYTES") && put(pickedText, "new text"));
        const QString mod = emu.filePath("load/0100ABCDEF012000/Mine/romfs");

        MainWindow w;
        w.setActiveMod(mod);
        w.show();
        QAction *replace = nullptr;
        for (QAction *a : w.findChildren<QAction *>())
            if (a->text() == "Replace file…")
                replace = a;
        QVERIFY(replace);

        auto run = [&](const QString &picked) {
            QString summary;
            QTimer poll;
            poll.setInterval(20);
            QEventLoop loop;
            connect(&poll, &QTimer::timeout, &loop, [&] {
                QWidget *modal = QApplication::activeModalWidget();
                if (auto *dialog = qobject_cast<QFileDialog *>(modal)) {
                    if (auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"))) {
                        name->setText(picked);
                        QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                    }
                } else if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                    summary = box->text();
                    box->accept();
                    loop.quit();
                }
            });
            poll.start();
            QTimer::singleShot(0, &loop, [&] { replace->trigger(); });
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            loop.exec();
            return summary;
        };
        auto read = [](const QString &path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<missing>");
        };

        QVERIFY(drop(w, plain));
        QTRY_VERIFY(replace->isEnabled());
        QString summary = run(pickedPlain);
        QVERIFY2(summary.startsWith("Saved data/plain.bin"), qPrintable(summary));
        QCOMPARE(read(mod + "/data/plain.bin"), QByteArray("NEW BYTES"));
        QCOMPARE(read(plain), QByteArray("OLD"));

        QVERIFY(drop(w, packed));
        QTRY_VERIFY(replace->isEnabled());
        summary = run(pickedText);
        QVERIFY2(summary.startsWith("Saved data/text.bin.zs"), qPrintable(summary));
        const QByteArray stored = read(mod + "/data/text.bin.zs");
        QCOMPARE(detectCompression(stored), Compression::Zstd);
        QCOMPARE(unpackAll(stored, nullptr, nullptr).value_or(QByteArray()), QByteArray("new text"));

        QVERIFY(drop(w, sarc));
        auto *table = w.findChild<QTableView *>();
        QTRY_COMPARE(table->model()->rowCount(), 2);
        for (int r = 0; r < table->model()->rowCount(); ++r)
            if (table->model()->index(r, 0).data().toString() == "inner.txt")
                emit table->activated(table->model()->index(r, 0));
        QTRY_VERIFY(replace->isEnabled() && w.findChild<QTableView *>()->model()->rowCount() != 2);
        summary = run(pickedText);
        QVERIFY2(summary.startsWith("Saved Pack/archive.sarc"), qPrintable(summary));
        const auto contents = readSarc(read(mod + "/Pack/archive.sarc"), nullptr);
        QVERIFY(contents);
        std::map<QString, QByteArray> files;
        for (const SarcEntry &e : contents->entries)
            files[e.name] = e.data;
        QCOMPARE(files["inner.txt"], QByteArray("new text"));
        QCOMPARE(files["keep.txt"], QByteArray("keep"));

        const std::vector<ModRecord> history = loadModHistory();
        QVERIFY(!history.empty());
        QCOMPARE(history.front().id, modId(mod));
        QCOMPARE(history.front().titleId, QString("0100ABCDEF012000"));
        QCOMPARE(history.front().lastSavedFile, QDir::cleanPath(mod + "/Pack/archive.sarc"));
        QVERIFY(history.front().lastSaved.isValid());
        QVERIFY(QSettings().value("modFolder").toString().isEmpty());

        MainWindow again;
        QVERIFY(again.activeModFolder().isEmpty());
        QSettings().remove("mods");
    }

    void filesInsideSwitchGamesCanBeReplacedIntoAMod()
    {
        QSettings().remove("mods");
        QTemporaryDir dir;
        const QString keys = switchsamples::writeFile(dir, "prod.keys", switchsamples::keysText());
        const QString game = switchsamples::writeFile(dir, "game.nsp", switchsamples::partition(switchsamples::packageFiles()));
        const QString picked = switchsamples::writeFile(dir, "picked.txt", "hello from the mod");
        const QString mod = dir.filePath("load/0100AAAA00000000/From NSP/romfs");

        MainWindow w;
        SwitchPackageReader::setKeyFile(keys);
        w.setActiveMod(mod);
        w.show();
        QVERIFY(drop(w, game));
        auto *table = w.findChild<QTableView *>();
        QTRY_COMPARE(table->model()->rowCount(), 4);
        for (int r = 0; r < table->model()->rowCount(); ++r)
            if (table->model()->index(r, 0).data().toString() == "Test Game [0100AAAA00000000]/romfs/sub/b.bin")
                emit table->activated(table->model()->index(r, 0));
        QAction *replace = nullptr;
        for (QAction *a : w.findChildren<QAction *>())
            if (a->text() == "Replace file…")
                replace = a;
        QTRY_VERIFY(replace->isEnabled() && table->model()->rowCount() != 4);

        QString summary;
        QTimer poll;
        poll.setInterval(20);
        QEventLoop loop;
        connect(&poll, &QTimer::timeout, &loop, [&] {
            QWidget *modal = QApplication::activeModalWidget();
            if (auto *dialog = qobject_cast<QFileDialog *>(modal)) {
                if (auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"))) {
                    name->setText(picked);
                    QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                }
            } else if (auto *box = qobject_cast<QMessageBox *>(modal)) {
                summary = box->text();
                box->accept();
                loop.quit();
            }
        });
        poll.start();
        QTimer::singleShot(0, &loop, [&] { replace->trigger(); });
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        QVERIFY2(summary.startsWith("Saved sub/b.bin"), qPrintable(summary));
        QFile saved(mod + "/sub/b.bin");
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), QByteArray("hello from the mod"));
        const std::vector<ModRecord> history = loadModHistory();
        QVERIFY(!history.empty());
        QCOMPARE(history.front().game, QString("Test Game"));
        SwitchPackageReader::setKeyFile({});
        QSettings().remove("mods");
    }

    void modManagerListsUndoesPacksAndDeletesMods()
    {
        QSettings().remove("mods");
        QTemporaryDir emu;
        auto put = [](const QString &path, const QByteArray &data) {
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile f(path);
            return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
        };
        const QString mine = emu.filePath("load/0100ABCDEF012000/Mine/romfs");
        QVERIFY(put(mine + "/a/one.bin", "1") && put(mine + "/a/two.bin", "22"));
        QVERIFY(put(emu.filePath("load/0100ABCDEF012000/Found/romfs/x.bin"), "x"));
        ModRecord record = modRecord(mine, "My Game");
        record.lastSavedFile = mine + "/a/two.bin";
        record.lastSaved = QDateTime::currentDateTime();
        rememberModInHistory(record);
        rememberModInHistory(modRecord(emu.filePath("load/0100ABCDEF012000/Gone/romfs")));

        ModManagerDialog dialog({QDir::cleanPath(emu.path())}, mine);
        dialog.setUseTrash(false);
        QStringList asked;
        dialog.setConfirmation([&](const QString &question, const QString &) {
            asked << question;
            return true;
        });
        QCOMPARE(dialog.modCount(), 3);
        QVERIFY(dialog.selectedMod());
        QCOMPARE(dialog.selectedMod()->id, modId(mine));
        QCOMPARE(dialog.selectedMod()->game, QString("My Game"));
        QCOMPARE(dialog.fileList(), QStringList({"romfs/a/one.bin", "romfs/a/two.bin"}));

        dialog.selectFiles({"romfs/a/one.bin"});
        QVERIFY(dialog.removeSelectedFiles());
        QVERIFY(!QFileInfo::exists(mine + "/a/one.bin"));
        QCOMPARE(dialog.fileList(), QStringList({"romfs/a/two.bin"}));

        const QString zip = emu.filePath("mine.zip");
        QVERIFY(dialog.packSelectedMod(zip));
        QVERIFY(QFileInfo(zip).size() > 22);

        QVERIFY(dialog.deleteSelectedMod());
        QVERIFY(!QFileInfo::exists(emu.filePath("load/0100ABCDEF012000/Mine")));
        QCOMPARE(asked.size(), 2);
        for (const ModRecord &r : loadModHistory())
            QVERIFY(r.id != modId(mine));

        QVERIFY(dialog.selectMod(modId(emu.filePath("load/0100ABCDEF012000/Gone/romfs"))));
        QVERIFY(!dialog.selectedMod()->exists());
        dialog.forgetMissing();
        QVERIFY(loadModHistory().empty());
        QCOMPARE(dialog.modCount(), 1);

        QVERIFY(dialog.selectMod(modId(emu.filePath("load/0100ABCDEF012000/Found/romfs"))));
        QPushButton *use = nullptr;
        for (QPushButton *b : dialog.findChildren<QPushButton *>())
            if (b->text() == "Use for saving")
                use = b;
        QVERIFY(use && use->isEnabled());
        use->click();
        QCOMPARE(dialog.chosenMod(), QDir::cleanPath(emu.filePath("load/0100ABCDEF012000/Found/romfs")));
        QSettings().remove("mods");
    }

    void oldModFolderSettingBecomesHistory()
    {
        QSettings().remove("mods");
        QTemporaryDir emu;
        const QString old = emu.filePath("load/0100ABCDEF012000/Old/romfs");
        QVERIFY(QDir().mkpath(old));
        QSettings().setValue("modFolder", old);
        MainWindow w;
        QVERIFY(w.activeModFolder().isEmpty());
        QVERIFY(!QSettings().contains("modFolder"));
        const std::vector<ModRecord> history = loadModHistory();
        QCOMPARE(history.size(), size_t(1));
        QCOMPARE(history.front().folder, QDir::cleanPath(old));
        QSettings().setValue("modFolder", emu.filePath("load/0100ABCDEF012000/Gone/romfs"));
        QCOMPARE(loadModHistory().size(), size_t(1));
        QSettings().remove("mods");
    }

    void newModBuildsTheFolderFromTheDumpPath()
    {
        QTemporaryDir emu;
        QVERIFY(QDir(emu.path()).mkpath("dump/0100ABCDEF012000/romfs/data"));
        const QString file = emu.filePath("dump/0100ABCDEF012000/romfs/data/items.tbl");
        {
            QFile f(file);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(sample::makeSample());
        }
        QVERIFY(QDir(emu.path()).mkpath("load/0100ABCDEF012000/Old mod/romfs/data"));
        const QString kept = emu.filePath("load/0100ABCDEF012000/Old mod/romfs/data/keep.bin");
        {
            QFile f(kept);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("old");
        }
        QSettings().remove("mods");
        QSettings().remove("newMod");

        MainWindow w;
        w.show();
        QVERIFY(drop(w, file));
        QAction *newMod = nullptr;
        for (QAction *a : w.findChildren<QAction *>())
            if (a->text() == "New mod…")
                newMod = a;
        QVERIFY(newMod);

        auto run = [&](const QString &name, QString *buttonText, QString *titleId, QString *base) {
            QTimer poll;
            poll.setInterval(20);
            QEventLoop loop;
            connect(&poll, &QTimer::timeout, &loop, [&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!dialog || dialog->windowTitle() != "New mod")
                    return;
                QLineEdit *nameEdit = nullptr, *idEdit = nullptr, *baseEdit = nullptr;
                for (QLineEdit *e : dialog->findChildren<QLineEdit *>()) {
                    if (e->placeholderText().startsWith("Shown in"))
                        nameEdit = e;
                    else if (e->placeholderText().startsWith("16 hex"))
                        idEdit = e;
                    else if (e->placeholderText().startsWith("The folder that holds load"))
                        baseEdit = e;
                }
                if (!nameEdit || !idEdit || !baseEdit)
                    return;
                *titleId = idEdit->text();
                *base = baseEdit->text();
                nameEdit->setText(name);
                QPushButton *ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
                *buttonText = ok->text();
                if (ok->isEnabled())
                    ok->click();
                else
                    dialog->reject();
                loop.quit();
            });
            poll.start();
            QTimer::singleShot(0, &loop, [&] { newMod->trigger(); });
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            loop.exec();
        };

        QString button, id, base;
        run("bad/name", &button, &id, &base);
        QCOMPARE(id, QString("0100ABCDEF012000"));
        QCOMPARE(QDir::fromNativeSeparators(base), QDir::cleanPath(emu.path()));
        QVERIFY(w.activeModFolder().isEmpty());

        run("New mod", &button, &id, &base);
        QCOMPARE(button, QString("Create"));
        QCOMPARE(w.activeModFolder(), emu.filePath("load/0100ABCDEF012000/New mod/romfs"));
        QVERIFY(QFileInfo(emu.filePath("load/0100ABCDEF012000/New mod/romfs")).isDir());

        run("Old mod", &button, &id, &base);
        QCOMPARE(button, QString("Use this folder"));
        QCOMPARE(w.activeModFolder(), emu.filePath("load/0100ABCDEF012000/Old mod/romfs"));
        QFile f(kept);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("old"));

        QSettings().remove("mods");
        QSettings().remove("newMod");
    }
};

QTEST_MAIN(TestMainWindow)
#include "test_mainwindow.moc"
