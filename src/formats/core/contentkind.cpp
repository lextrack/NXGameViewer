#include "core/contentkind.h"

#include "archives/decompress.h"

#include <QObject>

#include <array>

namespace {

struct Kind
{
    const char *magic;
    const char *name;
    ContentCategory category;
    int length = -1;

    QByteArrayView bytes() const { return length < 0 ? QByteArrayView(magic) : QByteArrayView(magic, length); }
};

constexpr std::array<Kind, 36> kKinds = {{
    {"@UTF", "CRI @UTF table", ContentCategory::Data},
    {"AFS2", "CRI AFS2 sound archive", ContentCategory::Sound},
    {"\x01\x00\x00\x80", "Nintendo Opus sound", ContentCategory::Sound, 4},
    {"\x00\x01\x00\x00\x00\xFF\xFF\xFF\xFF", ".NET BinaryFormatter data", ContentCategory::Data, 9},
    {"UnityFS", "Unity bundle", ContentCategory::Archive},
    {"NXGVUnityObject", "Unity object", ContentCategory::Data},
    {"SARC", "SARC archive", ContentCategory::Archive},
    {"FRES", "BFRES model", ContentCategory::Model},
    {"BNTX", "BNTX textures", ContentCategory::Texture},
    {"MsgStdBn", "MSBT text", ContentCategory::Text},
    {"MsgPrjBn", "MSBP project", ContentCategory::Text},
    {"YB", "BYML data", ContentCategory::Data},
    {"BY", "BYML data", ContentCategory::Data},
    {"FLYT", "BFLYT layout", ContentCategory::Data},
    {"FLAN", "BFLAN animation", ContentCategory::Data},
    {"FLIM", "BFLIM image", ContentCategory::Texture},
    {"FCPX", "BFCPX", ContentCategory::Data},
    {"FFNT", "BFFNT font", ContentCategory::Texture},
    {"BFEVFL", "BFEV event flow", ContentCategory::Data},
    {"BWAV", "BWAV sound", ContentCategory::Sound},
    {"BARS", "BARS sound pack", ContentCategory::Sound},
    {"FSAR", "BFSAR sound archive", ContentCategory::Sound},
    {"BNSH", "BNSH shaders", ContentCategory::Data},
    {"AAMP", "AAMP parameters", ContentCategory::Data},
    {"ASB ", "ASB animation graph", ContentCategory::Data},
    {"BDLI", "BDLI", ContentCategory::Data},
    {"MDL ", "Falcom model", ContentCategory::Model},
    {"DFvN", "XTX texture", ContentCategory::Texture},
    {"foxn", "Falcom sound", ContentCategory::Sound},
    {"JSON", "Falcom binary JSON", ContentCategory::Data},
    {"\x89PNG", "PNG image", ContentCategory::Image},
    {"\xFF\xD8\xFF", "JPEG image", ContentCategory::Image},
    {"DDS ", "DDS texture", ContentCategory::Texture},
    {"RIFF", "RIFF (WAV)", ContentCategory::Sound},
    {"OggS", "Ogg sound", ContentCategory::Sound},
    {"VSCB", "BCSV table", ContentCategory::Data},
}};

const Kind *findKind(QByteArrayView head)
{
    for (const Kind &k : kKinds)
        if (head.startsWith(k.bytes()))
            return &k;
    if (head.size() >= 16 && head.sliced(12, 4) == "VSCB")
        return &kKinds.back();
    return nullptr;
}

}

QString describeContent(QByteArrayView head)
{
    if (const Compression c = detectCompression(head); c != Compression::None)
        return QObject::tr("%1 compressed").arg(compressionName(c));
    if (const Kind *k = findKind(head))
        return QString::fromLatin1(k->name);
    if (head.isEmpty())
        return QObject::tr("empty");
    return QObject::tr("data");
}

ContentCategory contentCategory(QByteArrayView head)
{
    if (detectCompression(head) != Compression::None)
        return ContentCategory::Compressed;
    if (const Kind *k = findKind(head))
        return k->category;
    return ContentCategory::Other;
}
