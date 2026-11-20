#pragma once

#include <QByteArray>
#include <QString>

#include <functional>
#include <memory>

class QIODevice;

class ZipWriter
{
public:
    explicit ZipWriter(QIODevice *out);
    ~ZipWriter();

    ZipWriter(const ZipWriter &) = delete;
    ZipWriter &operator=(const ZipWriter &) = delete;

    bool addFile(const QString &name, QIODevice *source, const std::function<bool()> &keepGoing = {});
    bool addBytes(const QString &name, const QByteArray &data);
    bool finish();
    QString error() const;

private:
    struct State;
    std::unique_ptr<State> m;
};
