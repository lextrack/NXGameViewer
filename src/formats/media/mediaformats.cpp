#include "media/mediaformats.h"
#include "media/audiofilereader.h"
#include "media/fontfilereader.h"
#include "media/videofilereader.h"
#include "media/imagefilereader.h"

std::vector<std::unique_ptr<FormatReader>> mediaFormatReaders()
{
    std::vector<std::unique_ptr<FormatReader>> readers;
    readers.push_back(std::make_unique<ImageFileReader>());
    readers.push_back(std::make_unique<AudioFileReader>());
    readers.push_back(std::make_unique<FontFileReader>());
    readers.push_back(std::make_unique<VideoFileReader>());
    return readers;
}
