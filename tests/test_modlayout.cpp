#include "core/zipwriter.h"
#include "modding/modlayout.h"
#include "modding/modlibrary.h"
#include "switchfs/switchkeysearch.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <map>

#include <zlib.h>

namespace {

void write(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(data);
}

std::map<QString, QByteArray> unzip(const QByteArray &zip)
{
    std::map<QString, QByteArray> out;
    const qsizetype end = zip.lastIndexOf(QByteArray("PK\x05\x06", 4));
    if (end < 0 || end + 22 > zip.size())
        return out;
    const quint16 count = qFromLittleEndian<quint16>(zip.constData() + end + 10);
    qsizetype at = qFromLittleEndian<quint32>(zip.constData() + end + 16);
    for (int i = 0; i < count; ++i) {
        if (qFromLittleEndian<quint32>(zip.constData() + at) != 0x02014b50)
            return {};
        const quint32 crc = qFromLittleEndian<quint32>(zip.constData() + at + 16);
        const quint32 packed = qFromLittleEndian<quint32>(zip.constData() + at + 20);
        const quint32 size = qFromLittleEndian<quint32>(zip.constData() + at + 24);
        const quint16 nameLength = qFromLittleEndian<quint16>(zip.constData() + at + 28);
        const quint16 extra = qFromLittleEndian<quint16>(zip.constData() + at + 30);
        const quint16 comment = qFromLittleEndian<quint16>(zip.constData() + at + 32);
        const quint32 local = qFromLittleEndian<quint32>(zip.constData() + at + 42);
        const QString name = QString::fromUtf8(zip.mid(at + 46, nameLength));
        const quint16 localName = qFromLittleEndian<quint16>(zip.constData() + local + 26);
        const quint16 localExtra = qFromLittleEndian<quint16>(zip.constData() + local + 28);
        if (qFromLittleEndian<quint32>(zip.constData() + local + 14) != crc)
            return {};
        QByteArray data(qsizetype(size), '\0');
        z_stream z{};
        inflateInit2(&z, -15);
        z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(zip.constData() + local + 30 + localName + localExtra));
        z.avail_in = packed;
        z.next_out = reinterpret_cast<Bytef *>(data.data());
        z.avail_out = size;
        const int result = inflate(&z, Z_FINISH);
        inflateEnd(&z);
        if (result != Z_STREAM_END || crc32(0, reinterpret_cast<const Bytef *>(data.constData()), size) != crc)
            return {};
        out[name] = data;
        at += 46 + nameLength + extra + comment;
    }
    return out;
}

}

class TestModLayout : public QObject
{
    Q_OBJECT

private slots:
    void keepsAHistoryOfMods()
    {
        const QString yuzu = "/emu/load/01007A2027548000/Xbox buttons/romfs";
        const ModRecord r = modRecord(yuzu, "Trails");
        QCOMPARE(r.id.size(), 8);
        QCOMPARE(r.id, modId(yuzu + "/"));
        QCOMPARE(r.name, QString("Xbox buttons"));
        QCOMPARE(r.titleId, QString("01007A2027548000"));
        QCOMPARE(r.layout, ModLayout::YuzuFamily);
        QCOMPARE(modRecord("/r/mods/contents/0100F2C0115B6000/Big/romfs").layout, ModLayout::Ryujinx);
        QCOMPARE(modRecord("/sd/atmosphere/contents/0100F2C0115B6000/romfs").layout, ModLayout::Atmosphere);
        QVERIFY(r.id != modRecord("/r/mods/contents/0100F2C0115B6000/Big/romfs").id);

        ModRecord saved = r;
        saved.lastSavedFile = yuzu + "/a.bin";
        saved.lastSaved = QDateTime(QDate(2026, 11, 1), QTime(10, 0));
        const std::optional<ModRecord> back = modRecordFromVariant(modRecordToVariant(saved));
        QVERIFY(back);
        QCOMPARE(back->id, saved.id);
        QCOMPARE(back->game, QString("Trails"));
        QCOMPARE(back->lastSavedFile, saved.lastSavedFile);
        QCOMPARE(back->lastSaved, saved.lastSaved);
        QVERIFY(!modRecordFromVariant({}));

        std::vector<ModRecord> history;
        rememberMod(history, saved);
        rememberMod(history, modRecord("/r/mods/contents/0100F2C0115B6000/Big/romfs"));
        rememberMod(history, modRecord(yuzu));
        QCOMPARE(history.size(), size_t(2));
        QCOMPARE(history.front().id, r.id);
        QCOMPARE(history.front().game, QString("Trails"));
        QCOMPARE(history.front().lastSaved, saved.lastSaved);
        forgetMod(history, r.id);
        QCOMPARE(history.size(), size_t(1));
        forgetMissingMods(history);
        QVERIFY(history.empty());
        for (int i = 0; i < kMaxModHistory + 10; ++i)
            rememberMod(history, modRecord(QStringLiteral("/e/load/0100000000010000/m%1/romfs").arg(i)));
        QCOMPARE(history.size(), size_t(kMaxModHistory));
    }

    void findsGameNamesInPaths()
    {
        QCOMPARE(gameNameFromPath("/g/Red Dead Redemption [01007820196A6000][v0][US].nsp", "01007820196A6000"),
                 QString("Red Dead Redemption"));
        QCOMPARE(gameNameFromPath("/x/Test Game [0100AAAA00000000]/romfs/a.txt", "0100aaaa00000000"), QString("Test Game"));
        QCOMPARE(gameNameFromPath("/g/Animal Crossing [01006F8002326000]/u.nsp", "01006F8002326800"),
                 QString("Animal Crossing"));
        QVERIFY(gameNameFromPath("/g/dump/01007820196A6000/romfs/x", "01007820196A6000").isEmpty());
        QVERIFY(gameNameFromPath("/g/a [01007820196A6000]", "nope").isEmpty());
    }

    void findsInstalledModsAndTheirFiles()
    {
        QTemporaryDir root;
        KeySearchPlaces places;
        places.home = root.filePath("home");
        places.dataHome = root.filePath("home/.local/share");
        places.configHome = root.filePath("home/.config");
        write(root.filePath("home/.local/share/eden/load/01007A2027548000/Buttons/romfs/ui/a.bntx"), "AAAA");
        write(root.filePath("home/.local/share/eden/load/01007A2027548000/Buttons/romfs/ui/deep/b.bin"), "BB");
        write(root.filePath("home/.local/share/eden/load/01007A2027548000/60fps/exefs/patch.pchtxt"), "P");
        write(root.filePath("home/.local/share/eden/load/notatitle/Junk/romfs/x"), "x");
        write(root.filePath("home/.local/share/eden/sdmc/atmosphere/contents/0100F2C0115B6000/romfs/c"), "C");
        write(root.filePath("home/.config/Ryujinx/mods/contents/0100F2C0115B6000/Ryu mod/romfs/d"), "D");
        write(root.filePath("home/.var/app/org.citron_emu.citron/data/citron/load/0100F2C0115B6000/Flat/romfs/e"), "E");

        const QStringList folders = emulatorDataFolders(places);
        QVERIFY(folders.contains(QDir::cleanPath(root.filePath("home/.local/share/eden"))));
        QVERIFY(folders.contains(QDir::cleanPath(root.filePath("home/.config/Ryujinx"))));
        QVERIFY(folders.contains(QDir::cleanPath(root.filePath("home/.var/app/org.citron_emu.citron/data/citron"))));
        const std::vector<ModRecord> mods = findInstalledMods(folders);
        QStringList names;
        for (const ModRecord &m : mods)
            names << m.name + "|" + m.emulatorName();
        names.sort();
        QCOMPARE(names, QStringList({"60fps|yuzu family", "Atmosphère|Atmosphère", "Buttons|yuzu family",
                                     "Flat|yuzu family", "Ryu mod|Ryujinx"}));
        for (const ModRecord &m : mods)
            QVERIFY2(m.exists(), qPrintable(m.folder));

        const QString buttons = root.filePath("home/.local/share/eden/load/01007A2027548000/Buttons/romfs");
        std::vector<ModFile> files = listModFiles(buttons);
        QCOMPARE(files.size(), size_t(2));
        QCOMPARE(files[0].path, QString("romfs/ui/a.bntx"));
        QCOMPARE(files[1].path, QString("romfs/ui/deep/b.bin"));
        QCOMPARE(files[0].size, qint64(4));
        QCOMPARE(listModFiles(root.filePath("home/.local/share/eden/load/01007A2027548000/60fps/romfs")).front().path,
                 QString("exefs/patch.pchtxt"));

        QString error;
        QVERIFY(!removeModFile(buttons, "romfs/../../x", false, &error));
        QVERIFY(!removeModFile(buttons, "other/a", false, &error));
        QVERIFY(!removeModFile(buttons, "romfs/missing", false, &error));
        QVERIFY2(removeModFile(buttons, "romfs/ui/deep/b.bin", false, &error), qPrintable(error));
        QVERIFY(!QFileInfo::exists(buttons + "/ui/deep"));
        QVERIFY(QFileInfo(buttons + "/ui").isDir());
        QCOMPARE(listModFiles(buttons).size(), size_t(1));

        QVERIFY(!deleteMod(root.filePath("home/.local/share/eden/load"), false, &error));
        QVERIFY2(deleteMod(buttons, false, &error), qPrintable(error));
        QVERIFY(!QFileInfo::exists(root.filePath("home/.local/share/eden/load/01007A2027548000/Buttons")));
        QVERIFY(QFileInfo::exists(root.filePath("home/.local/share/eden/load/01007A2027548000/60fps")));
        const QString atmosphere = root.filePath("home/.local/share/eden/sdmc/atmosphere/contents/0100F2C0115B6000/romfs");
        QVERIFY2(deleteMod(atmosphere, false, &error), qPrintable(error));
        QVERIFY(!QFileInfo::exists(atmosphere));
        QVERIFY(QFileInfo::exists(QFileInfo(atmosphere).path()));
    }

    void packsModsAsZip()
    {
        QTemporaryDir root;
        const QString mod = root.filePath("load/01007A2027548000/My Mod/romfs");
        QByteArray big;
        for (int i = 0; i < 3000000; ++i)
            big += char('a' + (i * 7 + i / 1000) % 26);
        write(mod + "/ui/a.txt", "hello");
        write(mod + "/big.bin", big);
        write(mod + "/empty", {});
        write(root.filePath("load/01007A2027548000/My Mod/exefs/p.pchtxt"), "patch");
        QString error;
        const QString zip = root.filePath("out.zip");
        int calls = 0;
        QVERIFY2(packModZip(modRecord(mod), zip, [&](int, int) { return ++calls > 0; }, &error), qPrintable(error));
        QVERIFY(calls > 0);
        QFile f(zip);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const auto entries = unzip(f.readAll());
        QCOMPARE(entries.size(), size_t(4));
        QCOMPARE(entries.at("My Mod/romfs/ui/a.txt"), QByteArray("hello"));
        QCOMPARE(entries.at("My Mod/romfs/big.bin"), big);
        QCOMPARE(entries.at("My Mod/romfs/empty"), QByteArray());
        QCOMPARE(entries.at("My Mod/exefs/p.pchtxt"), QByteArray("patch"));

        const QString sd = root.filePath("atmosphere/contents/0100F2C0115B6000/romfs");
        write(sd + "/x.bin", "x");
        QVERIFY2(packModZip(modRecord(sd), root.filePath("sd.zip"), {}, &error), qPrintable(error));
        QFile g(root.filePath("sd.zip"));
        QVERIFY(g.open(QIODevice::ReadOnly));
        QCOMPARE(unzip(g.readAll()).count("atmosphere/contents/0100F2C0115B6000/romfs/x.bin"), size_t(1));

        QVERIFY(!packModZip(modRecord(root.filePath("load/01007A2027548000/Nothing/romfs")), root.filePath("n.zip"), {}, &error));
        QVERIFY(!packModZip(modRecord(mod), root.filePath("c.zip"), [](int, int) { return false; }, &error));
        QVERIFY(!QFileInfo::exists(root.filePath("c.zip")));

        QBuffer sequential;
        QVERIFY(sequential.open(QIODevice::WriteOnly));
        ZipWriter empty(&sequential);
        QVERIFY(empty.finish());
        QCOMPARE(sequential.data().size(), 22);
    }

    void recognisesTitleIds()
    {
        QVERIFY(isTitleId(u"01007A2027548000"));
        QVERIFY(isTitleId(u"0400486024dec000"));
        QVERIFY(!isTitleId(u"01007A202754800"));
        QVERIFY(!isTitleId(u"01007A20275480000"));
        QVERIFY(!isTitleId(u"01007A202754800G"));
        QVERIFY(!isTitleId(u""));
    }

    void findsTitleIdInPath()
    {
        QCOMPARE(titleIdFromPath("/games/dump/01007a2027548000/romfs/asset/x.xtx"), QString("01007A2027548000"));
#ifdef Q_OS_WIN
        QCOMPARE(titleIdFromPath("C:\\dumps\\0100ABCDEF012000\\romfs\\a.bin"), QString("0100ABCDEF012000"));
#endif
        QCOMPARE(titleIdFromPath("/a/1111111111111111/b/0100000000001000/romfs"), QString("0100000000001000"));
        QCOMPARE(titleIdFromPath("/a/1111111111111111/b/2222222222222222/c"), QString("2222222222222222"));
        QCOMPARE(titleIdFromPath("/home/me/files/textures/icons.xtx"), std::nullopt);
        QCOMPARE(titleIdFromPath(""), std::nullopt);
    }

    void suggestsEmulatorFolderNextToDump()
    {
        QCOMPARE(suggestedEmulatorFolder("/emu/dump/01007A2027548000/romfs/a"), QString("/emu"));
        QCOMPARE(suggestedEmulatorFolder("/emu/games/01007A2027548000/romfs/a"), QString());
        QCOMPARE(suggestedEmulatorFolder("/dump/01007A2027548000"), QString());
    }

    void buildsEachLayout_data()
    {
        QTest::addColumn<int>("layout");
        QTest::addColumn<QString>("base");
        QTest::addColumn<QString>("expected");
        const int yuzu = int(ModLayout::YuzuFamily), ryu = int(ModLayout::Ryujinx), atmo = int(ModLayout::Atmosphere);
        QTest::newRow("yuzu") << yuzu << "/emu" << "/emu/load/0100ABCDEF012000/My mod/romfs";
        QTest::newRow("yuzu given load") << yuzu << "/emu/load/" << "/emu/load/0100ABCDEF012000/My mod/romfs";
        QTest::newRow("ryujinx") << ryu << "/ryu" << "/ryu/mods/contents/0100ABCDEF012000/My mod/romfs";
        QTest::newRow("ryujinx given mods") << ryu << "/ryu/mods" << "/ryu/mods/contents/0100ABCDEF012000/My mod/romfs";
        QTest::newRow("ryujinx given contents") << ryu << "/ryu/mods/contents"
                                                << "/ryu/mods/contents/0100ABCDEF012000/My mod/romfs";
        QTest::newRow("atmosphere") << atmo << "/sd" << "/sd/atmosphere/contents/0100ABCDEF012000/romfs";
        QTest::newRow("atmosphere given folder") << atmo << "/sd/atmosphere"
                                                 << "/sd/atmosphere/contents/0100ABCDEF012000/romfs";
#ifdef Q_OS_WIN
        QTest::newRow("windows separators") << yuzu << "C:\\emu" << "C:/emu/load/0100ABCDEF012000/My mod/romfs";
#endif
    }

    void buildsEachLayout()
    {
        QFETCH(int, layout);
        QFETCH(QString, base);
        QFETCH(QString, expected);
        const ModLocation location{ModLayout(layout), base, "0100abcdef012000", "  My mod "};
        QCOMPARE(modRomfsPath(location), expected);
        const std::optional<ModLocation> parsed = parseModFolder(expected);
        QVERIFY(parsed);
        QCOMPARE(int(parsed->layout), layout);
        QCOMPARE(parsed->titleId, QString("0100ABCDEF012000"));
        QCOMPARE(parsed->modName, layoutHasModName(ModLayout(layout)) ? QString("My mod") : QString());
        QCOMPARE(modRomfsPath(*parsed), expected);
    }

    void acceptsTheGamesOwnModFolderAsBase()
    {
        QCOMPARE(titleIdFolder(ModLayout::YuzuFamily, "/emu/load/0100abcdef012000"), QString("0100ABCDEF012000"));
        QCOMPARE(titleIdFolder(ModLayout::Ryujinx, "/ryu/mods/contents/0100ABCDEF012000/"), QString("0100ABCDEF012000"));
        QCOMPARE(titleIdFolder(ModLayout::Atmosphere, "/sd/atmosphere/contents/0100ABCDEF012000"),
                 QString("0100ABCDEF012000"));
        QCOMPARE(titleIdFolder(ModLayout::YuzuFamily, "/emu/dump/0100ABCDEF012000"), std::nullopt);
        QCOMPARE(titleIdFolder(ModLayout::Ryujinx, "/ryu/contents/0100ABCDEF012000"), std::nullopt);
        QCOMPARE(titleIdFolder(ModLayout::YuzuFamily, "/emu"), std::nullopt);
        QCOMPARE(modRomfsPath({ModLayout::YuzuFamily, "/emu/load/0100ABCDEF012000", "0100ABCDEF012000", "My mod"}),
                 QString("/emu/load/0100ABCDEF012000/My mod/romfs"));
        QCOMPARE(modRomfsPath({ModLayout::Ryujinx, "/ryu/mods/contents/0100ABCDEF012000", "0100ABCDEF012000", "My mod"}),
                 QString("/ryu/mods/contents/0100ABCDEF012000/My mod/romfs"));
        QCOMPARE(modRomfsPath({ModLayout::Atmosphere, "/sd/atmosphere/contents/0100ABCDEF012000", "0100ABCDEF012000", {}}),
                 QString("/sd/atmosphere/contents/0100ABCDEF012000/romfs"));
    }

    void relatesUpdatesAndDlcToTheirGame()
    {
        QCOMPARE(applicationTitleId("0100abcdef012800"), QString("0100ABCDEF012000"));
        QCOMPARE(applicationTitleId("0100ABCDEF012000"), QString("0100ABCDEF012000"));
        QVERIFY(sameGame("0100ABCDEF012000", "0100abcdef012000"));
        QVERIFY(sameGame("0100ABCDEF012000", "0100ABCDEF012800"));
        QVERIFY(sameGame("0100ABCDEF012000", "0100ABCDEF013001"));
        QVERIFY(!sameGame("0100ABCDEF012000", "0100ABCDEF014000"));
        QVERIFY(!sameGame("0100ABCDEF012000", "0100000000010000"));
        QVERIFY(!sameGame("0100ABCDEF012000", "nope"));
    }

    void rejectsOtherFolders()
    {
        QCOMPARE(parseModFolder("/somewhere/romfs").has_value(), false);
        QCOMPARE(parseModFolder("/emu/load/0100ABCDEF012000/mod").has_value(), false);
        QCOMPARE(parseModFolder("/emu/other/0100ABCDEF012000/mod/romfs").has_value(), false);
        QCOMPARE(parseModFolder("/emu/load/not-a-title-id/mod/romfs").has_value(), false);
        QCOMPARE(parseModFolder("").has_value(), false);
        QCOMPARE(parseModFolder("romfs").has_value(), false);
    }

    void reusesExistingFolderCase()
    {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("load/0100abcdef012000/My Mod/romfs"));
        const ModLocation location{ModLayout::YuzuFamily, dir.path(), "0100ABCDEF012000", "my mod"};
        QCOMPARE(modRomfsPath(location), dir.path() + "/load/0100abcdef012000/My Mod/romfs");
    }

    void checksModNames()
    {
        QVERIFY(modNameProblem("Xbox button prompts").isEmpty());
        QVERIFY(modNameProblem("Ñandú 2.0 (HD)").isEmpty());
        QVERIFY(!modNameProblem("").isEmpty());
        QVERIFY(!modNameProblem("   ").isEmpty());
        QVERIFY(!modNameProblem(".").isEmpty());
        QVERIFY(!modNameProblem("..").isEmpty());
        QVERIFY(!modNameProblem("a/b").isEmpty());
        QVERIFY(!modNameProblem("a\\b").isEmpty());
        QVERIFY(!modNameProblem("what?").isEmpty());
        QVERIFY(!modNameProblem("end.").isEmpty());
        QVERIFY(!modNameProblem(QString("tab\there")).isEmpty());
    }
};

QTEST_MAIN(TestModLayout)
#include "test_modlayout.moc"
