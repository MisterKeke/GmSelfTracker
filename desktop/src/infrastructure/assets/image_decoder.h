#pragma once

#include <QImage>
#include <QString>

namespace gmst {

class ImageDecoder {
public:
    static constexpr qsizetype maximumBytes = 5 * 1024 * 1024;
    static constexpr int maximumDimension = 6000;
    static constexpr qint64 maximumPixels = 16'000'000;

    static QImage decode(const QByteArray &encoded, QString *error = nullptr);
    static bool supportsWebp();
    static bool verifyWebpDecoder(QString *error = nullptr);
};

} // namespace gmst
