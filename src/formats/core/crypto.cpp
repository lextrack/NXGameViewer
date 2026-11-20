#include "core/crypto.h"

#include <openssl/evp.h>

#include <limits>
#include <memory>

namespace crypto {

namespace {

using Context = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

Context context()
{
    return Context(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
}

bool run(const EVP_CIPHER *cipher, QByteArrayView key, QByteArrayView iv, bool encrypt, const char *in, char *out,
         qint64 size)
{
    Context ctx = context();
    if (!ctx || EVP_CipherInit_ex(ctx.get(), cipher, nullptr, reinterpret_cast<const unsigned char *>(key.data()),
                                  iv.isEmpty() ? nullptr : reinterpret_cast<const unsigned char *>(iv.data()), encrypt ? 1 : 0)
                    != 1)
        return false;
    EVP_CIPHER_CTX_set_padding(ctx.get(), 0);
    constexpr qint64 kStep = qint64{1} << 30;
    for (qint64 done = 0; done < size;) {
        const int step = int(std::min(kStep, size - done));
        int written = 0;
        if (EVP_CipherUpdate(ctx.get(), reinterpret_cast<unsigned char *>(out + done), &written,
                             reinterpret_cast<const unsigned char *>(in + done), step)
            != 1)
            return false;
        done += step;
    }
    return true;
}

const EVP_CIPHER *ecbCipher(QByteArrayView key)
{
    switch (key.size()) {
    case 16: return EVP_aes_128_ecb();
    case 32: return EVP_aes_256_ecb();
    default: return nullptr;
    }
}

QByteArray ecb(QByteArrayView key, QByteArrayView data, bool encrypt)
{
    const EVP_CIPHER *cipher = ecbCipher(key);
    QByteArray out(data.size(), '\0');
    if (!cipher || data.size() % 16 != 0 || !run(cipher, key, {}, encrypt, data.data(), out.data(), data.size()))
        return {};
    return out;
}

QByteArray xts(QByteArrayView key, QByteArrayView data, quint64 firstSector, int sectorSize, bool encrypt)
{
    if (key.size() != 32 || sectorSize <= 0 || data.size() % sectorSize != 0)
        return {};
    QByteArray out(data.size(), '\0');
    char tweak[16];
    for (qint64 at = 0, sector = qint64(firstSector); at < data.size(); at += sectorSize, ++sector) {
        quint64 value = quint64(sector);
        for (int i = 15; i >= 0; --i) {
            tweak[i] = char(value & 0xFF);
            value >>= 8;
        }
        if (!run(EVP_aes_128_xts(), key, QByteArrayView(tweak, 16), encrypt, data.data() + at, out.data() + at, sectorSize))
            return {};
    }
    return out;
}

}

QByteArray ecbDecrypt(QByteArrayView key, QByteArrayView data)
{
    return ecb(key, data, false);
}

QByteArray ecbEncrypt(QByteArrayView key, QByteArrayView data)
{
    return ecb(key, data, true);
}

bool ecbApplyInPlace(QByteArrayView key, char *data, qint64 size, int rounds, bool encrypt)
{
    const EVP_CIPHER *cipher = ecbCipher(key);
    if (!cipher || size % 16 != 0 || rounds < 0)
        return false;
    for (int r = 0; r < rounds; ++r)
        if (!run(cipher, key, {}, encrypt, data, data, size))
            return false;
    return true;
}

QByteArray xtsDecrypt(QByteArrayView key, QByteArrayView data, quint64 firstSector, int sectorSize)
{
    return xts(key, data, firstSector, sectorSize, false);
}

QByteArray xtsEncrypt(QByteArrayView key, QByteArrayView data, quint64 firstSector, int sectorSize)
{
    return xts(key, data, firstSector, sectorSize, true);
}

bool ctrApply(QByteArrayView key, QByteArrayView counter, char *data, qint64 size)
{
    if (key.size() != 16 || counter.size() != 16)
        return false;
    return run(EVP_aes_128_ctr(), key, counter, true, data, data, size);
}

QByteArray ctrCounter(QByteArrayView upper, quint64 offset)
{
    QByteArray counter(16, '\0');
    for (int i = 0; i < 8 && i < upper.size(); ++i)
        counter[i] = upper[i];
    quint64 block = offset >> 4;
    for (int i = 15; i >= 8; --i) {
        counter[i] = char(block & 0xFF);
        block >>= 8;
    }
    return counter;
}

}
