#include "core/Checksum.h"

namespace immichksync {

std::optional<Sha1Checksum> Sha1Checksum::fromRaw(const QByteArray &bytes)
{
    if (bytes.size() != kByteCount) {
        return std::nullopt;
    }
    return Sha1Checksum(bytes);
}

std::optional<Sha1Checksum> Sha1Checksum::fromBase64(const QString &encoded)
{
    if (encoded.size() != kBase64Length) {
        return std::nullopt;
    }
    // AbortOnBase64DecodingErrors: a lenient decode would happily turn a 28-character
    // hex-ish string into 20 arbitrary bytes and hand back a plausible-looking digest.
    const auto decoded = QByteArray::fromBase64Encoding(
        encoded.toLatin1(), QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) {
        return std::nullopt;
    }
    return fromRaw(*decoded);
}

std::optional<Sha1Checksum> Sha1Checksum::fromHex(const QString &encoded)
{
    if (encoded.size() != kHexLength) {
        return std::nullopt;
    }
    for (const QChar c : encoded) {
        // QByteArray::fromHex silently skips anything that is not a hex digit, so the
        // alphabet has to be checked before the conversion rather than after it.
        if (!std::isxdigit(static_cast<unsigned char>(c.toLatin1()))) {
            return std::nullopt;
        }
    }
    return fromRaw(QByteArray::fromHex(encoded.toLatin1()));
}

std::optional<Sha1Checksum> Sha1Checksum::fromEncoded(const QString &encoded)
{
    if (auto value = fromBase64(encoded)) {
        return value;
    }
    return fromHex(encoded);
}

QString Sha1Checksum::hex() const
{
    return QString::fromLatin1(m_bytes.toHex());
}

QString Sha1Checksum::base64() const
{
    return QString::fromLatin1(m_bytes.toBase64());
}

QString Sha1Checksum::shortHex() const
{
    return hex().left(8);
}

} // namespace immichksync
