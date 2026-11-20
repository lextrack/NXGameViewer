#include "unity/lz4block.h"

#include <cstring>

std::optional<QByteArray> lz4BlockDecompress(QByteArrayView source, qsizetype decompressedSize)
{
    if (decompressedSize < 0)
        return std::nullopt;
    QByteArray out(decompressedSize, Qt::Uninitialized);
    const auto *in = reinterpret_cast<const uchar *>(source.data());
    const qsizetype inSize = source.size();
    char *dst = out.data();
    qsizetype ip = 0, op = 0;
    auto readLength = [&](qsizetype length) -> qsizetype {
        if (length != 15)
            return length;
        while (ip < inSize) {
            const uchar more = in[ip++];
            length += more;
            if (more != 255)
                return length;
        }
        return -1;
    };
    while (ip < inSize) {
        const uchar token = in[ip++];
        const qsizetype literals = readLength(token >> 4);
        if (literals < 0 || literals > inSize - ip || literals > decompressedSize - op)
            return std::nullopt;
        std::memcpy(dst + op, in + ip, size_t(literals));
        ip += literals;
        op += literals;
        if (ip == inSize)
            break;
        if (ip + 2 > inSize)
            return std::nullopt;
        const qsizetype offset = qsizetype(in[ip]) | (qsizetype(in[ip + 1]) << 8);
        ip += 2;
        if (offset == 0 || offset > op)
            return std::nullopt;
        const qsizetype match = readLength(token & 15);
        if (match < 0)
            return std::nullopt;
        const qsizetype length = match + 4;
        if (length > decompressedSize - op)
            return std::nullopt;
        for (qsizetype i = 0; i < length; ++i, ++op)
            dst[op] = dst[op - offset];
    }
    if (op != decompressedSize)
        return std::nullopt;
    return out;
}
