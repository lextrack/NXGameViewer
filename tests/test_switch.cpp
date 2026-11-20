#include "core/crypto.h"
#include "core/paths.h"
#include "core/registry.h"
#include "switchfs/switchgame.h"
#include "switchfs/switchkeys.h"
#include "switchfs/switchkeysearch.h"
#include "switchfs/switchnca.h"
#include "switchfs/switchpackagereader.h"
#include "switchfs/switchncz.h"
#include "switchfs/switchnup.h"
#include "switch_samples.h"

#include <QCryptographicHash>
#include <QFile>
#include <QThread>
#include <QDirIterator>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <atomic>
#include <cstring>
#include <map>
#include <vector>

using namespace switchsamples;

class TestSwitch : public QObject
{
    Q_OBJECT

private slots:
    void findsKeysAutomatically()
    {
        QTemporaryDir root;
        KeySearchPlaces places;
        places.home = root.filePath("home");
        places.dataHome = root.filePath("home/.local/share");
        places.configHome = root.filePath("home/.config");
        places.appData = root.filePath("AppData/Roaming");
        places.localAppData = root.filePath("AppData/Local");
        const QString game = writeFile(root, "games/game.nsp", partition(packageFiles(0x80, {}, true, 2)));
        writeFile(root, "games/readme.txt", "not a key file");
        places.gameFolders = KeySearchPlaces::foldersOf({game});

        QVERIFY(!findSwitchKeys({game}, places).found);
        QVERIFY(!findSwitchKeys({game}, places).problem.isEmpty());

        const QString old = writeFile(root, "home/.local/share/eden/keys/prod.keys", keysText(0));
        const QString good = writeFile(root, "home/.config/Ryujinx/system/prod.keys", keysText(2));
        const QString titles = writeFile(root, "home/.switch/title.keys", "0100bbbb00000000000000000000000b = " + kTitleKey.toHex() + "\n");
        writeFile(root, "home/.var/app/dev.eden_emu.eden/data/eden/keys/prod.keys", keysText(0));
        writeFile(root, "AppData/Roaming/citron/keys/prod.keys", keysText(0));
        writeFile(root, "emulators/yuzu/user/keys/prod.keys", keysText(0));
        writeFile(root, "games/sudachi/user/keys/prod.keys", keysText(0));

        const QStringList folders = switchKeyFolders(places);
        for (const char *expected : {"home/.local/share/eden/keys", "home/.config/Ryujinx/system", "home/.switch",
                                     "home/.var/app/dev.eden_emu.eden/data/eden/keys", "AppData/Roaming/citron/keys",
                                     "games/sudachi/user/keys"})
            QVERIFY2(folders.contains(QDir::cleanPath(root.filePath(expected))), expected);
        QVERIFY(folders.indexOf(QDir::cleanPath(root.filePath("games"))) == 0);
        QVERIFY(folders.indexOf(QDir::cleanPath(root.filePath("home/.switch"))) == folders.size() - 1);

        SwitchKeySearch search = findSwitchKeys({game}, places);
        QVERIFY(search.found);
        QCOMPARE(search.found->prodKeys, QFileInfo(good).absoluteFilePath());
        QVERIFY(search.found->usable);
        QVERIFY(search.problem.isEmpty());
        QCOMPARE(search.found->neededGeneration, 2);
        QCOMPARE(search.found->newestGeneration, 2);
        QVERIFY(search.found->titleKeyFiles.contains(QFileInfo(titles).absoluteFilePath()));
        QVERIFY(!search.found->keys.titleKey(QByteArray::fromHex("0100bbbb00000000000000000000000b")).isEmpty());

        const QString beside = writeFile(root, "games/my keys.txt", keysText(2));
        search = findSwitchKeys({game}, places);
        QCOMPARE(search.found->prodKeys, QFileInfo(beside).absoluteFilePath());

        QVERIFY(QFile::remove(beside));
        QVERIFY(QFile::remove(good));
        search = findSwitchKeys({game}, places);
        QVERIFY(search.found);
        QVERIFY(!search.found->usable);
        QVERIFY(search.found->readsGame);
        QVERIFY2(search.problem.contains("master_key_02") && search.problem.contains("master_key_00"),
                 qPrintable(search.problem));

        QByteArray wrong = keysText(2);
        wrong.replace(kHeaderKey.toHex(), QByteArray(32, 'a').toHex());
        const QString wrongFile = writeFile(root, "home/.config/Ryujinx/system/prod.keys", wrong);
        const SwitchKeySearch chosen = useSwitchKeyFile(wrongFile, {game}, places);
        QVERIFY(chosen.found);
        QVERIFY(!chosen.found->readsGame);
        QVERIFY(!chosen.found->usable);
        QVERIFY(chosen.problem.contains("header_key"));

        search = findSwitchKeys({}, places);
        QVERIFY(search.found);
        QVERIFY(search.found->usable);
        QVERIFY(!search.found->testedOnGame);
        QCOMPARE(search.found->newestGeneration, 2);
    }

    void readsNszAndXcz()
    {
        QByteArray big;
        QRandomGenerator random(7);
        for (int i = 0; i < 40000; ++i)
            big += QByteArray::number(i) + ',';
        for (int i = 0; i < 20000; ++i)
            big += char(random.bounded(256));
        const auto files = packageFiles(0x80, {}, true, 0, big);
        QTemporaryDir dir;
        for (int exponent : {0, 14}) {
            const auto compressed = nszFiles(files, exponent);
            bool anyNcz = false;
            for (const auto &[name, data] : compressed)
                anyNcz = anyNcz || name.endsWith(".ncz");
            QVERIFY(anyNcz);
            for (const auto &[name, data] : files) {
                if (!name.endsWith(".nca") || name.endsWith(".cnmt.nca") || data.size() <= 0x4000)
                    continue;
                const QByteArray plain = decryptedNca(data);
                StoragePtr storage = nczStorage(bytesStorage(ncz(data, exponent)), nullptr);
                QVERIFY(storage);
                QVERIFY(storage->decryptedSections());
                QCOMPARE(storage->size(), plain.size());
                for (qint64 at : {qint64(0xC00), plain.size() - 5000, qint64(0x3FF0), qint64(0x4000), qint64(0x5123),
                                  plain.size() / 2, qint64(0x1000), plain.size() - 1})
                    QCOMPARE(storage->read(at, std::min<qint64>(9000, plain.size() - at)),
                             plain.mid(at, std::min<qint64>(9000, plain.size() - at)));
                QVERIFY(storage->read(plain.size() - 4, 8).isEmpty());
            }

            const QString path = writeFile(dir, QStringLiteral("game%1.nsz").arg(exponent), partition(compressed));
            QString error;
            const std::vector<SwitchTitle> titles = scanSwitchPackage(path, testKeys(), &error);
            QVERIFY2(titles.size() == 1, qPrintable(error));
            const SwitchTitle &t = titles.front();
            QCOMPARE(t.name, QString("Test Game"));
            QVERIFY2(t.canExtract(), qPrintable(t.problem));
            QVERIFY2(verifySwitchTitle(t, testKeys(), {}, &error), qPrintable(error));
            const QString out = dir.filePath(QStringLiteral("out%1").arg(exponent));
            SwitchExtractResult result;
            QVERIFY2(extractSwitchTitle(t, testKeys(), out, {}, &result, &error), qPrintable(error));
            QCOMPARE(result.files, 4);
            auto read = [&](const QString &name) {
                QFile f(QDir(out).filePath(name));
                return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<missing>");
            };
            QCOMPARE(read("exefs/main"), QByteArray("MAIN-CODE"));
            QCOMPARE(read("romfs/a.txt"), QByteArray("hello"));
            QVERIFY(read("romfs/sub/b.bin") == big);
        }

        const QString xczPath = writeFile(dir, "game.xcz", xci(nszFiles(files, 15)));
        QString error;
        const std::vector<SwitchTitle> xcz = scanSwitchPackage(xczPath, testKeys(), &error);
        QVERIFY2(xcz.size() == 1 && xcz.front().canExtract(), qPrintable(error));
        QVERIFY2(verifySwitchTitle(xcz.front(), testKeys(), {}, &error), qPrintable(error));

        for (const auto &[name, data] : files) {
            if (!name.endsWith(".nca") || name.endsWith(".cnmt.nca") || data.size() <= 0x10000)
                continue;
            QByteArray broken = ncz(data, 14);
            broken.truncate(broken.size() - 100);
            StoragePtr cut = nczStorage(bytesStorage(broken), nullptr);
            QVERIFY(!cut);
            QByteArray noMagic = ncz(data, 0);
            noMagic[0x4000] = 'X';
            QVERIFY(!nczStorage(bytesStorage(noMagic), nullptr));
            QByteArray damaged = ncz(data, 0);
            damaged.truncate(damaged.size() - 50);
            StoragePtr partial = nczStorage(bytesStorage(damaged), nullptr);
            QVERIFY(partial);
            QVERIFY(partial->read(partial->size() - 16, 16).isEmpty());
        }
    }

    void browsesPackagesAsArchives()
    {
        QTemporaryDir dir;
        const QString keys = writeFile(dir, "prod.keys", keysText());
        const QString nsp = writeFile(dir, "game.nsp", partition(packageFiles()));
        const QString update = writeFile(dir, "update.nsp", partition(packageFiles(0x81)));
        const QString xciPath = writeFile(dir, "game.xci", xci(packageFiles()));
        const QString nszPath = writeFile(dir, "game.nsz", partition(nszFiles(packageFiles(0x80, {}, true, 0, QByteArray(300000, 'Z')), 0)));

        SwitchPackageReader reader;
        QVERIFY(reader.score("game.nsp", {}) > 0 && reader.score("GAME.XCZ", {}) > 0 && reader.score("a.bin", {}) == 0);
        SwitchPackageReader::setKeyFile({});
        QVERIFY(SwitchPackageReader::needsKeys(nsp));
        QString error;
        QVERIFY(!reader.read(nsp, &error));
        QVERIFY2(error.contains("keys"), qPrintable(error));

        SwitchPackageReader::setKeyFile(keys);
        QVERIFY(!SwitchPackageReader::needsKeys(nsp));
        for (const QString &path : {nsp, xciPath}) {
            const auto doc = reader.read(path, &error);
            QVERIFY2(doc, qPrintable(error));
            const auto archive = doc->tables.front()->archive();
            QVERIFY(archive);
            const QString folder = "Test Game [0100AAAA00000000]/";
            for (const QString &name : {folder + "exefs/main", folder + "exefs/main.npdm", folder + "romfs/a.txt",
                                        folder + "romfs/sub/b.bin"})
                QVERIFY2(archive->find(name) >= 0, qPrintable(name));
            QCOMPARE(archive->entries.size(), size_t(4));
            const size_t main = size_t(archive->find(folder + "exefs/main"));
            QCOMPARE(archive->entryData(main), QByteArray("MAIN-CODE"));
            QCOMPARE(archive->entryHead(main, 4), QByteArray("MAIN"));
            const size_t b = size_t(archive->find(folder + "romfs/sub/b.bin"));
            QCOMPARE(archive->entries[b].size, qsizetype(3000));
            QCOMPARE(archive->entryReader(b)(1000, 10), QByteArray(10, 'B'));
            QVERIFY(archive->entryReader(b)(2995, 10).isEmpty());
            QCOMPARE(doc->tables.front()->text(0, 2, DisplayMode::Auto), QString("Game"));
        }

        const auto updateDoc = reader.read(update, &error);
        QVERIFY2(updateDoc, qPrintable(error));
        const auto updateArchive = updateDoc->tables.front()->archive();
        QCOMPARE(updateArchive->entries.size(), size_t(1));
        QCOMPARE(updateArchive->entries[0].name, QString("Test Game [0100AAAA00000000]"));
        QVERIFY(!QString::fromUtf8(updateArchive->entryData(0)).isEmpty());
        QCOMPARE(updateDoc->tables.front()->text(0, 2, DisplayMode::Auto), QString("Update"));
        QVERIFY(!updateDoc->tables.front()->text(0, 3, DisplayMode::Auto).isEmpty());
        QVERIFY(updateDoc->tables.front()->description().contains("cannot be read yet"));

        const auto nszDoc = reader.read(nszPath, &error);
        QVERIFY2(nszDoc, qPrintable(error));
        const auto nszArchive = nszDoc->tables.front()->archive();
        const int big = nszArchive->find("Test Game [0100AAAA00000000]/romfs/sub/b.bin");
        QVERIFY(big >= 0);
        QVERIFY(nszArchive->entryHead(size_t(big), 4).isEmpty());
        QCOMPARE(nszArchive->entryData(size_t(big)), QByteArray(300000, 'Z'));

        FormatRegistry registry;
        const FormatReader *used = nullptr;
        QVERIFY2(registry.open(nsp, &used, &error), qPrintable(error));
        QCOMPARE(used->name(), reader.name());
        SwitchPackageReader::setKeyFile({});
    }

    void parsesAndDerivesKeys()
    {
        QString error;
        const auto keys = SwitchKeys::fromText(keysText(), "0100aaaa00000000000000000000000a = " + kTitleKey.toHex() + "\n",
                                               &error);
        QVERIFY2(keys, qPrintable(error));
        QCOMPARE(keys->headerKey(), kHeaderKey);
        QCOMPARE(keys->keyAreaKey(0, 0), kAreaKey);
        QCOMPARE(keys->titlekek(0), kTitlekek);
        QVERIFY(keys->keyAreaKey(1, 0).isEmpty());
        QCOMPARE(keys->titleKey(kRightsId), kTitleKey);

        const QByteArray master = QByteArray::fromHex("00112233445566778899aabbccddeeff");
        const QByteArray kekSeed = QByteArray::fromHex("4d870986c45d20722fba1053da92e8a9");
        const QByteArray keySeed = QByteArray::fromHex("89615ee05c31b6805fe58f3da24f7aa8");
        const QByteArray areaSource = QByteArray::fromHex("7f59971e629f36a13098066f2144c30d");
        const QByteArray titlekekSource = QByteArray::fromHex("1edc7b3b60e6b4d878b81715985e629b");
        const QByteArray headerKekSource = QByteArray::fromHex("1f12913a4acbf00d4cde3af6d523882a");
        const QByteArray headerKeySource = QByteArray(32, char(0x5A));
        const QByteArray text = "master_key_00 = " + master.toHex() + "\naes_kek_generation_source = " + kekSeed.toHex()
                              + "\naes_key_generation_source = " + keySeed.toHex() + "\nkey_area_key_application_source = "
                              + areaSource.toHex() + "\ntitlekek_source = " + titlekekSource.toHex()
                              + "\nheader_kek_source = " + headerKekSource.toHex() + "\nheader_key_source = "
                              + headerKeySource.toHex() + "\n";
        const auto derived = SwitchKeys::fromText(text, {}, &error);
        QVERIFY2(derived, qPrintable(error));
        auto kek = [&](const QByteArray &source) {
            const QByteArray k = crypto::ecbDecrypt(master, kekSeed);
            return crypto::ecbDecrypt(crypto::ecbDecrypt(k, source), keySeed);
        };
        QCOMPARE(derived->keyAreaKey(0, 0), kek(areaSource));
        QCOMPARE(derived->titlekek(0), crypto::ecbDecrypt(master, titlekekSource));
        QCOMPARE(derived->headerKey(), crypto::ecbDecrypt(kek(headerKekSource), headerKeySource));

        QVERIFY(!SwitchKeys::fromText("titlekek_00 = 00\n", {}, &error));
        QVERIFY(error.contains("header_key"));
    }

    void cryptoRoundTrips()
    {
        const QByteArray data = QByteArray(0x400, 'x') + QByteArray(0x200, 'y');
        const QByteArray encrypted = crypto::xtsEncrypt(kHeaderKey, data, 0, 0x200);
        QCOMPARE(encrypted.size(), data.size());
        QVERIFY(encrypted.mid(0, 0x200) != encrypted.mid(0x200, 0x200));
        QCOMPARE(crypto::xtsDecrypt(kHeaderKey, encrypted, 0, 0x200), data);
        QCOMPARE(crypto::xtsDecrypt(kHeaderKey, encrypted.mid(0x200), 1, 0x200), data.mid(0x200));
        QCOMPARE(crypto::ctrCounter(QByteArray::fromHex("0102030405060708"), 0x1230).toHex(),
                 QByteArray("01020304050607080000000000000123"));
        QByteArray text = "counter mode works on any length";
        QVERIFY(crypto::ctrApply(kAreaKey, QByteArray(16, '\0'), text.data(), text.size()));
        QVERIFY(text != "counter mode works on any length");
        QVERIFY(crypto::ctrApply(kAreaKey, QByteArray(16, '\0'), text.data(), text.size()));
        QCOMPARE(text, QByteArray("counter mode works on any length"));
    }

    void scansAndExtractsNsp()
    {
        QTemporaryDir dir;
        const QString path = writeFile(dir, "game.nsp", partition(packageFiles()));
        QString error;
        const std::vector<SwitchTitle> titles = scanSwitchPackage(path, testKeys(), &error);
        QVERIFY2(titles.size() == 1, qPrintable(error));
        const SwitchTitle &t = titles.front();
        QCOMPARE(t.type, SwitchTitle::Application);
        QCOMPARE(t.titleIdText(), QString("0100AAAA00000000"));
        QCOMPARE(t.version, 65536u);
        QCOMPARE(t.name, QString("Test Game"));
        QCOMPARE(t.publisher, QString("Tester"));
        QCOMPARE(t.displayVersion, QString("1.2.3"));
        QVERIFY2(t.canExtract(), qPrintable(t.problem));

        QVERIFY2(verifySwitchTitle(t, testKeys(), {}, &error), qPrintable(error));

        const QString out = dir.filePath("out");
        SwitchExtractResult result;
        qint64 lastDone = -1, lastTotal = 0;
        QVERIFY2(extractSwitchTitle(t, testKeys(), out,
                                    [&](qint64 done, qint64 total) {
                                        lastDone = done;
                                        lastTotal = total;
                                        return true;
                                    },
                                    &result, &error),
                 qPrintable(error));
        QCOMPARE(result.files, 4);
        QCOMPARE(lastDone, lastTotal);
        auto read = [&](const QString &name) {
            QFile f(QDir(out).filePath(name));
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<missing>");
        };
        QCOMPARE(read("exefs/main"), QByteArray("MAIN-CODE"));
        QCOMPARE(read("exefs/main.npdm"), QByteArray("NPDM"));
        QCOMPARE(read("romfs/a.txt"), QByteArray("hello"));
        QCOMPARE(read("romfs/sub/b.bin"), QByteArray(3000, 'B'));
    }

    void readsXci()
    {
        QTemporaryDir dir;
        std::vector<std::pair<QByteArray, QByteArray>> files = packageFiles();
        const QString path = writeFile(dir, "game.xci", xci(files));
        QString error;
        const std::vector<SwitchTitle> titles = scanSwitchPackage(path, testKeys(), &error);
        QVERIFY2(titles.size() == 1, qPrintable(error));
        QCOMPARE(titles.front().name, QString("Test Game"));
        QVERIFY2(titles.front().canExtract(), qPrintable(titles.front().problem));
        const auto list = switchTitleFiles(titles.front(), testKeys(), &error);
        QVERIFY2(list, qPrintable(error));
        QCOMPARE(list->size(), size_t(4));
    }

    void extractsFilesInParallelSafely()
    {
        QTemporaryDir dir;
        QString error;
        std::vector<QByteArray> contents;
        for (int i = 0; i < 300; ++i) {
            QByteArray data(i % 7 == 0 ? 0 : 1000 + i * 37, '\0');
            for (qsizetype k = 0; k < data.size(); ++k)
                data[k] = char((k * 13 + i) % 251);
            contents.push_back(data);
        }
        auto inFlight = std::make_shared<std::atomic<int>>(0);
        auto most = std::make_shared<std::atomic<int>>(0);
        auto failAt = std::make_shared<std::atomic<int>>(-1);
        auto makeFiles = [&](const QByteArray &tag, bool sequential) {
            std::vector<SwitchTitleFile> files;
            for (int i = 0; i < int(contents.size()); ++i) {
                const QByteArray data = contents[size_t(i)] + tag;
                files.push_back({QStringLiteral("romfs/d%1/f%2.bin").arg(i % 5).arg(i), data.size(),
                                 [data, i, inFlight, most, failAt](qint64 offset, char *out, qint64 length) {
                                     const int now = ++*inFlight;
                                     int seen = most->load();
                                     while (now > seen && !most->compare_exchange_weak(seen, now)) {
                                     }
                                     QThread::usleep(50);
                                     const bool ok = failAt->load() != i && offset >= 0 && offset + length <= data.size();
                                     if (ok)
                                         std::memcpy(out, data.constData() + offset, size_t(length));
                                     --*inFlight;
                                     return ok;
                                 },
                                 true, sequential});
            }
            return files;
        };
        auto partials = [&](const QString &folder) {
            int n = 0;
            QDirIterator it(folder, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext())
                n += it.next().endsWith(".nxgv-partial") ? 1 : 0;
            return n;
        };
        auto check = [&](const QString &folder, const QByteArray &tag) {
            for (int i = 0; i < int(contents.size()); ++i) {
                QFile f(QDir(folder).filePath(QStringLiteral("romfs/d%1/f%2.bin").arg(i % 5).arg(i)));
                if (!f.open(QIODevice::ReadOnly) || f.readAll() != contents[size_t(i)] + tag)
                    return false;
            }
            return true;
        };
        const QString out = dir.filePath("out");
        qint64 total = 0;
        for (const QByteArray &c : contents)
            total += c.size();
        qint64 last = -1;
        bool ordered = true;
        SwitchExtractResult result;
        QVERIFY2(extractSwitchFiles(makeFiles({}, false), out,
                                    [&](qint64 done, qint64 all) {
                                        ordered = ordered && done >= last && all == total;
                                        last = done;
                                        return true;
                                    },
                                    &result, &error),
                 qPrintable(error));
        QVERIFY(ordered);
        QCOMPARE(last, total);
        QCOMPARE(result.files, 300);
        QCOMPARE(result.bytes, total);
        QVERIFY(check(out, {}));
        QCOMPARE(partials(out), 0);

        QVERIFY2(extractSwitchFiles(makeFiles("v2", false), out, {}, &result, &error), qPrintable(error));
        QVERIFY(check(out, "v2"));

        *most = 0;
        QVERIFY2(extractSwitchFiles(makeFiles({}, true), dir.filePath("seq"), {}, &result, &error), qPrintable(error));
        QCOMPARE(most->load(), 1);
        QVERIFY(check(dir.filePath("seq"), {}));

        qint64 budget = total / 3;
        QVERIFY(extractSwitchFiles(makeFiles({}, false), dir.filePath("cancel"), [&](qint64 done, qint64) { return done < budget; },
                                   &result, &error));
        QVERIFY(result.canceled);
        QVERIFY(result.files < 300);
        QCOMPARE(partials(dir.filePath("cancel")), 0);

        *failAt = 150;
        QVERIFY(!extractSwitchFiles(makeFiles({}, false), dir.filePath("broken"), {}, &result, &error));
        QVERIFY2(error.contains("f150.bin"), qPrintable(error));
        QCOMPARE(partials(dir.filePath("broken")), 0);
    }

    void cancelsAndReportsProblems()
    {
        QTemporaryDir dir;
        QString error;
        const QString path = writeFile(dir, "game.nsp", partition(packageFiles()));
        const SwitchTitle t = scanSwitchPackage(path, testKeys(), &error).front();
        SwitchExtractResult result;
        QVERIFY(extractSwitchTitle(t, testKeys(), dir.filePath("out"), [](qint64, qint64) { return false; }, &result, &error));
        QVERIFY(result.canceled);

        const QString noTicket = writeFile(dir, "noticket.nsp", partition(packageFiles(0x80, {}, false)));
        const auto missing = scanSwitchPackage(noTicket, testKeys(), &error);
        QCOMPARE(missing.size(), size_t(1));
        QVERIFY(missing.front().problem.contains("ticket"));

        const QString update = writeFile(dir, "update.nsp", partition(packageFiles(0x81)));
        const auto patch = scanSwitchPackage(update, testKeys(), &error);
        QCOMPARE(patch.front().type, SwitchTitle::Patch);
        QVERIFY(!patch.front().canExtract());
        QVERIFY(!extractSwitchTitle(patch.front(), testKeys(), dir.filePath("never"), {}, nullptr, &error));
        QVERIFY(!QDir(dir.filePath("never")).exists());

        SwitchKeys wrong = *SwitchKeys::fromText("header_key = " + QByteArray(32, char(0x42)).toHex() + "\n", {}, &error);
        QVERIFY(scanSwitchPackage(path, wrong, &error).empty());

        const QString unsafe = writeFile(dir, "unsafe.nsp", partition(packageFiles(0x80, "..")));
        const SwitchTitle bad = scanSwitchPackage(unsafe, testKeys(), &error).front();
        QVERIFY(!extractSwitchTitle(bad, testKeys(), dir.filePath("unsafe"), {}, nullptr, &error));
        QVERIFY(error.contains("unsafe"));
        QVERIFY(!QFile::exists(dir.filePath("a.txt")));
    }

    void extractsDlcRomfsOnly()
    {
        QTemporaryDir dir;
        QString error;
        const QString path = writeFile(dir, "dlc.nsp", partition(packageFiles(0x82)));
        const auto titles = scanSwitchPackage(path, testKeys(), &error);
        QCOMPARE(titles.size(), size_t(1));
        QCOMPARE(titles.front().type, SwitchTitle::AddOn);
        QVERIFY2(titles.front().canExtract(), qPrintable(titles.front().problem));
        SwitchExtractResult result;
        QVERIFY2(extractSwitchTitle(titles.front(), testKeys(), dir.filePath("dlc"), {}, &result, &error), qPrintable(error));
        QVERIFY(QFile::exists(dir.filePath("dlc/romfs/a.txt")));
        QVERIFY(!QDir(dir.filePath("dlc/exefs")).exists());
    }

    void extractsCompressedRomfs()
    {
        QTemporaryDir dir;
        QString error;
        QByteArray big = QByteArray(0x1000, '\0') + QByteArray(3000, 'C');
        for (qsizetype i = 0x1000; i < big.size(); i += 5)
            big[i] = char(i);
        const QString path = writeFile(dir, "compressed.nsp", partition(packageFiles(0x80, {}, true, 0, big, true)));
        const std::vector<SwitchTitle> titles = scanSwitchPackage(path, testKeys(), &error);
        QVERIFY2(titles.size() == 1, qPrintable(error));
        const SwitchTitle &t = titles.front();
        QVERIFY2(t.canExtract(), qPrintable(t.problem));
        QVERIFY2(verifySwitchTitle(t, testKeys(), {}, &error), qPrintable(error));
        SwitchExtractResult result;
        const QString out = dir.filePath("out");
        QVERIFY2(extractSwitchTitle(t, testKeys(), out, {}, &result, &error), qPrintable(error));
        auto read = [&](const QString &name) {
            QFile f(QDir(out).filePath(name));
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<missing>");
        };
        QCOMPARE(read("romfs/a.txt"), QByteArray("hello"));
        QCOMPARE(read("romfs/sub/b.bin"), big);
    }

    void readsNupContainers()
    {
        QTemporaryDir dir;
        QString error;
        QByteArray big(300000, '\0');
        for (qsizetype i = 0; i < big.size(); ++i)
            big[i] = char((i * 7) % 251);
        NupPackageSpec base{0x0400AAAA00000000ull, 0x10000, 0,
                            {{"exefs/main", "NSO0-MAIN", false},
                             {"exefs/main.npdm", "META", true},
                             {"romfs/a.txt", "hello", false},
                             {"romfs/sub/b.bin", big, true},
                             {"romfs/sub/empty.dat", {}, false}},
                            {{5, "Nup Game"}, {6, "Tester"}, {4, "1.0.0"}, {3, "\xff\xd8\xff\xe0jpeg"}}};
        NupPackageSpec dlc{0x0400AAAA00001001ull, 0, 2, {{"romfs/dlc.txt", "extra", true}}, {}};
        const QByteArray image = nupFile({base, dlc});
        const QString path = writeFile(dir, "game.nup", image);
        QVERIFY(isNupFile(path));

        const std::vector<SwitchTitle> titles = scanSwitchPackage(path, SwitchKeys(), &error);
        QVERIFY2(titles.size() == 2, qPrintable(error));
        QCOMPARE(titles[0].type, SwitchTitle::Application);
        QCOMPARE(titles[0].titleIdText(), QString("0400AAAA00000000"));
        QCOMPARE(titles[0].name, QString("Nup Game"));
        QCOMPARE(titles[0].publisher, QString("Tester"));
        QCOMPARE(titles[0].displayVersion, QString("1.0.0"));
        QVERIFY(titles[0].icon.startsWith("\xff\xd8"));
        QCOMPARE(titles[1].type, SwitchTitle::AddOn);
        QCOMPARE(titles[1].name, QString("Nup Game"));
        QVERIFY2(titles[0].canExtract() && titles[1].canExtract(), qPrintable(titles[0].problem + titles[1].problem));
        QVERIFY(checkSwitchPackageKeys(path, SwitchKeys()).works());

        QVERIFY2(verifySwitchTitle(titles[0], SwitchKeys(), {}, &error), qPrintable(error));
        SwitchExtractResult result;
        const QString out = dir.filePath("out");
        QVERIFY2(extractSwitchTitle(titles[0], SwitchKeys(), out, {}, &result, &error), qPrintable(error));
        QCOMPARE(result.files, 5);
        auto read = [&](const QString &name) {
            QFile f(QDir(out).filePath(name));
            return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<missing>");
        };
        QCOMPARE(read("exefs/main"), QByteArray("NSO0-MAIN"));
        QCOMPARE(read("exefs/main.npdm"), QByteArray("META"));
        QCOMPARE(read("romfs/a.txt"), QByteArray("hello"));
        QCOMPARE(read("romfs/sub/b.bin"), big);
        QCOMPARE(read("romfs/sub/empty.dat"), QByteArray());

        const auto files = switchTitleFiles(titles[0], SwitchKeys(), &error);
        QVERIFY(files);
        for (const SwitchTitleFile &f : *files)
            if (f.path == "romfs/sub/b.bin") {
                QByteArray middle(1000, '\0');
                QVERIFY(f.read(200000, middle.data(), middle.size()));
                QCOMPARE(middle, big.mid(200000, 1000));
                QVERIFY(f.read(10, middle.data(), 100));
                QCOMPARE(middle.left(100), big.mid(10, 100));
            }

        SwitchPackageReader::setKeyFile({});
        QVERIFY(!SwitchPackageReader::needsKeys(path));
        auto doc = SwitchPackageReader().read(path, &error);
        QVERIFY2(doc, qPrintable(error));
        const auto archive = doc->tables.front()->archive();
        QVERIFY(archive);
        bool sawDlc = false;
        for (size_t i = 0; i < archive->entries.size(); ++i)
            if (archive->entries[i].name.endsWith("/romfs/dlc.txt")) {
                sawDlc = true;
                QCOMPARE(archive->entryData(i), QByteArray("extra"));
            }
        QVERIFY(sawDlc);

        QByteArray badToc = image;
        badToc[0x90] = char(badToc[0x90] ^ 1);
        QVERIFY(scanSwitchPackage(writeFile(dir, "badtoc.nup", badToc), SwitchKeys(), &error).empty());
        QVERIFY2(error.contains("checksum"), qPrintable(error));

        QByteArray badData = image;
        const qsizetype hello = badData.indexOf("hello");
        QVERIFY(hello > 0);
        badData[hello] = 'j';
        const QString badPath = writeFile(dir, "baddata.nup", badData);
        const auto badTitles = scanSwitchPackage(badPath, SwitchKeys(), &error);
        QVERIFY(!badTitles.empty());
        QVERIFY(!verifySwitchTitle(badTitles[0], SwitchKeys(), {}, &error));
        QVERIFY2(error.contains("checksum"), qPrintable(error));
    }

    void detectsDamagedData()
    {
        QTemporaryDir dir;
        QString error;
        std::vector<std::pair<QByteArray, QByteArray>> files = packageFiles();
        QByteArray &program = files.front().second;
        program[program.size() - 0x300] = char(program[program.size() - 0x300] ^ 0x55);
        const QString path = writeFile(dir, "damaged.nsp", partition(files));
        const SwitchTitle t = scanSwitchPackage(path, testKeys(), &error).front();
        QVERIFY(!verifySwitchTitle(t, testKeys(), {}, &error));
        QVERIFY(error.contains("hash"));

        QVERIFY(!readPartition(bytesStorage("PFS0\xff\xff\xff\x7f\0\0\0\0\0\0\0\0"), &error));
        QVERIFY(scanSwitchPackage(writeFile(dir, "junk.nsp", "not a package"), testKeys(), &error).empty());
        QVERIFY(!isSafeRelativePath("../x"));
        QVERIFY(!isSafeRelativePath("/abs"));
        QVERIFY(!isSafeRelativePath("a//b"));
        QVERIFY(isSafeRelativePath("Data/level0"));
        QVERIFY(!isSafeRelativePath("C:/Windows/x"));
        QCOMPARE(sanitizedRelativePath("C:/Windows/x"), QString("C_/Windows/x"));
        QCOMPARE(sanitizedRelativePath("..\\..\\evil.txt"), QString("evil.txt"));
        QCOMPARE(sanitizedRelativePath("/abs/./a/../b"), QString("abs/a/b"));
        QCOMPARE(sanitizedRelativePath("a/b.bin"), QString("a/b.bin"));
        QCOMPARE(safeFileName(".."), QString("__"));
        QCOMPARE(safeFileName(" . "), QString("_"));
        QCOMPARE(safeFileName("a..b"), QString("a..b"));
    }
};

QTEST_GUILESS_MAIN(TestSwitch)
#include "test_switch.moc"
