#pragma once

#include <QByteArrayView>
#include <QString>

#include <vector>

enum class TextEncoding { Utf8, Utf16LE, Utf16BE, ShiftJis, EucKr, Gbk, Big5, Windows1252 };

QString encodingName(TextEncoding encoding);
const std::vector<TextEncoding> &allTextEncodings();

struct DetectedEncoding
{
    TextEncoding encoding = TextEncoding::Utf8;
    int bomSize = 0;
    bool confident = false;
};

DetectedEncoding detectTextEncoding(QByteArrayView text);
DetectedEncoding detectEncodingInBinary(QByteArrayView data);

QString decodeText(QByteArrayView bytes, TextEncoding encoding, bool *ok = nullptr);
int characterLength(QByteArrayView data, qsizetype pos, TextEncoding encoding);
QString characterText(QByteArrayView data, qsizetype pos, TextEncoding encoding, int *length);
bool looksLikeText(QByteArrayView sample);
