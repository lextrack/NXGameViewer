#include "media/imagefilereader.h"
#include "core/simpletable.h"

#include <QBuffer>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QLocale>
#include <QObject>

#include <cstring>

namespace {

bool isContentDetectable(const QByteArray &format)
{
    return format == "png" || format == "jpeg" || format == "jpg" || format == "gif" || format == "bmp"
        || format == "webp";
}

std::shared_ptr<Image> toImage(const QImage &source)
{
    const QImage rgba = source.convertToFormat(QImage::Format_RGBA8888);
    auto image = std::make_shared<Image>();
    image->width = rgba.width();
    image->height = rgba.height();
    const qsizetype rowBytes = qsizetype(rgba.width()) * 4;
    image->rgba.resize(rowBytes * rgba.height());
    for (int y = 0; y < rgba.height(); ++y)
        std::memcpy(image->rgba.data() + y * rowBytes, rgba.constScanLine(y), size_t(rowBytes));
    return image;
}

}

int ImageFileReader::score(const QString &fileName, QByteArrayView head) const
{
    QByteArray bytes = head.toByteArray();
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    if (isContentDetectable(QImageReader::imageFormat(&buffer)))
        return 90;
    const QByteArray suffix = QFileInfo(fileName).suffix().toLower().toLatin1();
    if (!suffix.isEmpty() && QImageReader::supportedImageFormats().contains(suffix))
        return 50;
    return 0;
}

std::unique_ptr<Document> ImageFileReader::read(const QString &path, QString *error) const
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QByteArray format = reader.format();
    const int frameCount = reader.supportsAnimation() ? reader.imageCount() : 1;

    std::vector<QImage> frames;
    while (int(frames.size()) < kMaxFrames) {
        QImage frame = reader.read();
        if (frame.isNull())
            break;
        frames.push_back(std::move(frame));
        if (!reader.supportsAnimation() || !reader.canRead())
            break;
    }
    if (frames.empty()) {
        if (error)
            *error = QObject::tr("Not a readable image: %1").arg(reader.errorString());
        return nullptr;
    }

    const QLocale locale;
    const QString fileSize = locale.formattedDataSize(QFileInfo(path).size());
    auto doc = std::make_unique<Document>();
    for (size_t i = 0; i < frames.size(); ++i) {
        const QImage &frame = frames[i];
        const std::vector<SimpleTable::Row> rows = {
            {QObject::tr("format"), QString::fromLatin1(format).toUpper()},
            {QObject::tr("width"), frame.width()},
            {QObject::tr("height"), frame.height()},
            {QObject::tr("transparency"), frame.hasAlphaChannel() ? QObject::tr("yes") : QObject::tr("no")},
            {QObject::tr("bits per pixel"), frame.depth()},
            {QObject::tr("frames"), frameCount > 0 ? frameCount : int(frames.size())},
            {QObject::tr("file size"), fileSize},
        };
        const QString name = frames.size() > 1 ? QObject::tr("Frame %1").arg(i + 1) : QObject::tr("Image");
        auto table = std::make_shared<SimpleTable>(
            name, std::vector<SimpleColumn>{{QObject::tr("property")}, {QObject::tr("value")}}, rows);
        table->setImage(toImage(frame));
        table->setDescription(QObject::tr("%1 × %2 %3").arg(frame.width()).arg(frame.height())
                                  .arg(QString::fromLatin1(format).toUpper()));
        doc->tables.push_back(table);
    }
    return doc;
}
