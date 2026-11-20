#include "textures/astc.h"

#include <astcenc.h>

#include <QObject>
#include <QThreadPool>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <tuple>
#include <vector>

namespace {

struct ContextDeleter
{
    void operator()(astcenc_context *context) const { astcenc_context_free(context); }
};
using Context = std::unique_ptr<astcenc_context, ContextDeleter>;

Context makeContext(int blockWidth, int blockHeight, bool srgb, float quality, unsigned flags, unsigned threads,
                    QString *error)
{
    astcenc_config config;
    astcenc_error status = astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, unsigned(blockWidth),
                                               unsigned(blockHeight), 1, quality, flags, &config);
    astcenc_context *context = nullptr;
    if (status == ASTCENC_SUCCESS)
        status = astcenc_context_alloc(&config, threads, &context);
    if (status != ASTCENC_SUCCESS) {
        if (error)
            *error = QObject::tr("The ASTC encoder could not start: %1").arg(QString::fromUtf8(astcenc_get_error_string(status)));
        return nullptr;
    }
    return Context(context);
}

struct Decoder
{
    std::mutex mutex;
    Context context;
};

Decoder *decoderFor(int blockWidth, int blockHeight, bool srgb)
{
    static std::mutex mutex;
    static std::map<std::tuple<int, int, bool>, std::unique_ptr<Decoder>> decoders;
    const std::lock_guard lock(mutex);
    auto &slot = decoders[{blockWidth, blockHeight, srgb}];
    if (!slot) {
        auto decoder = std::make_unique<Decoder>();
        decoder->context = makeContext(blockWidth, blockHeight, srgb, ASTCENC_PRE_FASTEST, ASTCENC_FLG_DECOMPRESS_ONLY, 1,
                                       nullptr);
        if (!decoder->context)
            return nullptr;
        slot = std::move(decoder);
    }
    return slot.get();
}

const astcenc_swizzle kIdentity{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};

qsizetype blockBytes(int width, int height, int blockWidth, int blockHeight)
{
    return qsizetype((width + blockWidth - 1) / blockWidth) * ((height + blockHeight - 1) / blockHeight) * 16;
}

}

std::optional<QByteArray> decodeAstc(QByteArrayView blocks, int width, int height, int blockWidth, int blockHeight,
                                     bool srgb)
{
    if (width <= 0 || height <= 0 || blocks.size() < blockBytes(width, height, blockWidth, blockHeight))
        return std::nullopt;
    Decoder *decoder = decoderFor(blockWidth, blockHeight, srgb);
    if (!decoder)
        return std::nullopt;
    QByteArray out(qsizetype(width) * height * 4, '\0');
    void *slice = out.data();
    astcenc_image image{unsigned(width), unsigned(height), 1, ASTCENC_TYPE_U8, &slice};
    const std::lock_guard lock(decoder->mutex);
    const astcenc_error status = astcenc_decompress_image(
        decoder->context.get(), reinterpret_cast<const uint8_t *>(blocks.data()),
        size_t(blockBytes(width, height, blockWidth, blockHeight)), &image, &kIdentity, 0);
    astcenc_decompress_reset(decoder->context.get());
    if (status != ASTCENC_SUCCESS)
        return std::nullopt;
    return out;
}

std::optional<QByteArray> encodeAstc(const QByteArray &rgba, int width, int height, int blockWidth, int blockHeight,
                                     bool srgb, QString *error)
{
    if (width <= 0 || height <= 0 || rgba.size() < qsizetype(width) * height * 4) {
        if (error)
            *error = QObject::tr("The picture is empty.");
        return std::nullopt;
    }
    const unsigned busy = unsigned(std::max(1, QThreadPool::globalInstance()->activeThreadCount()));
    const unsigned threads = std::clamp(std::thread::hardware_concurrency() / busy, 1u, 32u);
    const qsizetype size = blockBytes(width, height, blockWidth, blockHeight);
    const float quality = size / 16 <= 65536 ? ASTCENC_PRE_THOROUGH : ASTCENC_PRE_MEDIUM;
    Context context = makeContext(blockWidth, blockHeight, srgb, quality, 0, threads, error);
    if (!context)
        return std::nullopt;

    QByteArray pixels = rgba;
    void *slice = pixels.data();
    astcenc_image image{unsigned(width), unsigned(height), 1, ASTCENC_TYPE_U8, &slice};
    QByteArray out(size, '\0');
    std::vector<astcenc_error> results(threads, ASTCENC_SUCCESS);
    {
        std::vector<std::jthread> workers;
        workers.reserve(threads - 1);
        unsigned started = 1;
        try {
            for (; started < threads; ++started)
                workers.emplace_back([&, i = started] {
                    results[i] = astcenc_compress_image(context.get(), &image, &kIdentity,
                                                        reinterpret_cast<uint8_t *>(out.data()), size_t(out.size()), i);
                });
        } catch (const std::system_error &) {
            results.resize(started);
        }
        results[0] = astcenc_compress_image(context.get(), &image, &kIdentity, reinterpret_cast<uint8_t *>(out.data()),
                                            size_t(out.size()), 0);
    }
    for (astcenc_error status : results)
        if (status != ASTCENC_SUCCESS) {
            if (error)
                *error = QObject::tr("ASTC compression failed: %1").arg(QString::fromUtf8(astcenc_get_error_string(status)));
            return std::nullopt;
        }
    return out;
}
