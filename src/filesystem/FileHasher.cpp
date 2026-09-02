#include "filesystem/FileHasher.h"

#include <QCryptographicHash>
#include <QFile>

namespace immichksync {

namespace FileHasher {

std::optional<Sha1Checksum> checksumOf(const QString &path, QString *errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return std::nullopt;
    }

    QCryptographicHash hash(QCryptographicHash::Sha1);
    // addData(QIODevice*) reads in chunks internally, so a 4 GiB video costs a buffer,
    // not 4 GiB of resident memory.
    if (!hash.addData(&file)) {
        if (errorMessage) {
            *errorMessage = file.errorString().isEmpty()
                ? QStringLiteral("Could not read the file while hashing it.")
                : file.errorString();
        }
        return std::nullopt;
    }
    return Sha1Checksum::fromRaw(hash.result());
}

} // namespace FileHasher

} // namespace immichksync
