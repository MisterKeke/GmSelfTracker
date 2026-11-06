#include "infrastructure/assets/image_decoder.h"

#include <QBuffer>
#include <QImageReader>

#include <cstring>
#include <cstdint>

#if GMST_HAS_WEBP
#include <webp/decode.h>
#include <webp/encode.h>
#endif

namespace gmst {
namespace {

bool validDimensions(int width, int height, QString *error)
{
    if (width <= 0 || height <= 0 || width > ImageDecoder::maximumDimension || height > ImageDecoder::maximumDimension ||
        static_cast<qint64>(width) * height > ImageDecoder::maximumPixels) {
        if (error) *error = QStringLiteral("Image dimensions exceed the 6000 × 6000 or 16-megapixel limit.");
        return false;
    }
    return true;
}

} // namespace

bool ImageDecoder::supportsWebp()
{
#if GMST_HAS_WEBP
    return true;
#else
    return false;
#endif
}

bool ImageDecoder::verifyWebpDecoder(QString *error)
{
#if GMST_HAS_WEBP
    const std::uint8_t sample[4] = {0x37, 0x8a, 0xd4, 0xff};
    std::uint8_t *encoded = nullptr;
    const std::size_t encodedSize = WebPEncodeLosslessRGBA(sample, 1, 1, 4, &encoded);
    if (encodedSize == 0 || encoded == nullptr) {
        if (error) *error = QStringLiteral("libwebp could not encode the disposable decoder probe.");
        return false;
    }
    const QByteArray bytes(reinterpret_cast<const char *>(encoded), static_cast<qsizetype>(encodedSize));
    WebPFree(encoded);
    QString decodeError;
    const QImage image = decode(bytes, &decodeError);
    if (image.size() != QSize(1, 1)) {
        if (error) *error = decodeError.isEmpty() ? QStringLiteral("The disposable WebP round-trip did not produce a 1 × 1 image.") : decodeError;
        return false;
    }
    return true;
#else
    if (error) *error = QStringLiteral("libwebp is not linked into this build.");
    return false;
#endif
}

QImage ImageDecoder::decode(const QByteArray &encoded, QString *error)
{
    if (encoded.isEmpty() || encoded.size() > maximumBytes) {
        if (error) *error = QStringLiteral("Image data must be between 1 byte and 5 MB.");
        return {};
    }

    const bool isWebp = encoded.size() >= 12 && encoded.startsWith("RIFF") && encoded.mid(8, 4) == "WEBP";
    if (isWebp) {
#if GMST_HAS_WEBP
        int width = 0;
        int height = 0;
        const auto *data = reinterpret_cast<const std::uint8_t *>(encoded.constData());
        if (!WebPGetInfo(data, static_cast<std::size_t>(encoded.size()), &width, &height)) {
            if (error) *error = QStringLiteral("The WebP image header is invalid.");
            return {};
        }
        if (!validDimensions(width, height, error)) return {};
        std::uint8_t *pixels = WebPDecodeBGRA(data, static_cast<std::size_t>(encoded.size()), &width, &height);
        if (!pixels) {
            if (error) *error = QStringLiteral("The WebP image could not be decoded.");
            return {};
        }
        QImage image(width, height, QImage::Format_ARGB32);
        for (int row = 0; row < height; ++row) {
            std::memcpy(image.scanLine(row), pixels + static_cast<std::size_t>(row) * width * 4,
                        static_cast<std::size_t>(width) * 4);
        }
        WebPFree(pixels);
        return image;
#else
        if (error) *error = QStringLiteral("WebP decoding is unavailable. Build with the libwebp dependency.");
        return {};
#endif
    }

    QBuffer buffer;
    buffer.setData(encoded);
    if (!buffer.open(QIODevice::ReadOnly)) {
        if (error) *error = buffer.errorString();
        return {};
    }
    QImageReader reader(&buffer);
    reader.setDecideFormatFromContent(true);
    const QByteArray format = reader.format().toLower();
    static const QList<QByteArray> allowed = {"png", "jpg", "jpeg", "gif", "bmp"};
    if (!allowed.contains(format)) {
        if (error) *error = QStringLiteral("Supported image formats are PNG, JPEG, GIF, WebP, and BMP.");
        return {};
    }
    const QSize dimensions = reader.size();
    if (!validDimensions(dimensions.width(), dimensions.height(), error)) return {};
    QImage image = reader.read();
    if (image.isNull() && error) *error = reader.errorString();
    return image;
}

} // namespace gmst
