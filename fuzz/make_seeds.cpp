#include "archives/compress.h"

#include "sample_bfres.h"
#include "sample_bjson.h"
#include "sample_bntx.h"
#include "sample_mdl.h"
#include "sample_rpf6.h"
#include "sample_sarc.h"
#include "sample_tbl.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>

#include <cstdio>
#include <utility>
#include <vector>

namespace {

QByteArray rgbaBntx()
{
    samplebntx::Texture texture;
    texture.name = "Seed";
    texture.width = 4;
    texture.height = 4;
    texture.linear = QByteArray(4 * 4 * 4, '\x7f');
    return samplebntx::make({texture});
}

}

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output folder>\n", argv[0]);
        return 2;
    }
    const QDir out(QString::fromLocal8Bit(argv[1]));
    if (!out.mkpath(QStringLiteral(".")))
        return 1;

    const QByteArray bntx = rgbaBntx();
    std::vector<std::pair<QString, QByteArray>> seeds = {
        {"items.tbl", sample::makeSample()},
        {"data.json", samplejson::make()},
        {"selfref.json", samplejson::makeSelfReferencingList(4)},
        {"model.mdl", sample::makeModel()},
        {"rich.mdl", sample::makeRichModel(true)},
        {"anim.mdl", sample::makeAnimation({}, 0.0f, 1.0f)},
        {"texture.bntx", bntx},
        {"model.bfres", samplebfres::make()},
        {"plain.txt", "hello\nworld\n"},
        {"object.json", R"({"a": [1, 2.5, "x"], "b": {"c": null}})"},
    };
    const QByteArray sarc = samplesarc::make({{"items.tbl", sample::makeSample()}, {"tex.bntx", bntx}, {"", "nameless"}});
    seeds.emplace_back("archive.sarc", sarc);
    seeds.emplace_back("archive_be.sarc", samplesarc::make({{"a.txt", "hello"}}, false));
    seeds.emplace_back("archive.szs", compressYaz0(sarc));
    seeds.emplace_back("plain.rpf", samplerpf6::makeArchive(false).file);
    QString error;
    if (auto zstd = compressZstd(sarc, 3, true, &error))
        seeds.emplace_back("archive.sarc.zs", *zstd);

    for (const auto &[name, data] : seeds) {
        QFile file(out.filePath(name));
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
            std::fprintf(stderr, "could not write %s\n", qPrintable(file.fileName()));
            return 1;
        }
    }
    std::printf("wrote %zu seeds to %s\n", seeds.size(), qPrintable(out.absolutePath()));
    return 0;
}
