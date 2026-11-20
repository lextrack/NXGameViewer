#pragma once

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
    bool finish();
    QString error() const;

private:
    struct State;
    std::unique_ptr<State> m;
};
