#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>

#include <optional>

namespace immichksync {

/// A SHA-1 digest of a file's contents — the only identifier a local file and a
/// remote asset share.
///
/// The server hands checksums back base64-encoded (`AssetResponseDto.checksum`) but
/// accepts either encoding on the way in. Wrapping the raw 20 bytes removes any
/// chance of comparing a hex string against a base64 one, which would silently make
/// every asset look new.
class Sha1Checksum {
public:
    static constexpr int kByteCount = 20;
    static constexpr int kHexLength = 40;
    static constexpr int kBase64Length = 28;

    Sha1Checksum() = default;

    /// Wraps exactly 20 raw digest bytes; returns nullopt for anything else.
    static std::optional<Sha1Checksum> fromRaw(const QByteArray &bytes);
    static std::optional<Sha1Checksum> fromBase64(const QString &encoded);
    static std::optional<Sha1Checksum> fromHex(const QString &encoded);
    /// Accepts whichever encoding the source used, discriminating on length.
    static std::optional<Sha1Checksum> fromEncoded(const QString &encoded);

    bool isNull() const { return m_bytes.isEmpty(); }
    QByteArray bytes() const { return m_bytes; }

    /// Lowercase hex, the form the official CLI sends.
    QString hex() const;
    /// Canonical storage form: matches what the API returns, and 28 chars beats 40.
    QString base64() const;
    /// Short, stable suffix used to disambiguate two assets with the same filename.
    QString shortHex() const;

    bool operator==(const Sha1Checksum &other) const { return m_bytes == other.m_bytes; }
    bool operator!=(const Sha1Checksum &other) const { return m_bytes != other.m_bytes; }
    /// Ordered by base64 so plans iterate deterministically, matching the macOS build.
    bool operator<(const Sha1Checksum &other) const { return base64() < other.base64(); }

private:
    explicit Sha1Checksum(QByteArray bytes) : m_bytes(std::move(bytes)) {}

    QByteArray m_bytes;
};

inline size_t qHash(const Sha1Checksum &checksum, size_t seed = 0)
{
    return qHash(checksum.bytes(), seed);
}

} // namespace immichksync
