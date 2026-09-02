#pragma once

#include <QString>

namespace immichksync {

/// Holding area for files whose asset left its album on the server.
///
/// The sync contract with the user is that this app never destroys local data: a
/// removal moves the file to `<root>/.immich-trash/<album folder>/` where it stays
/// until they decide otherwise. That single guarantee is what makes it safe to run
/// two-way sync unattended.
///
/// Deliberately not the XDG trash: that is the user's, it can be emptied by anything,
/// and a file put there is much harder to correlate back to the album it came from.
class LocalTrash {
public:
    explicit LocalTrash(QString rootPath);

    QString directory() const;

    /// Moves a file into the trash, returning where it landed (empty on failure).
    QString moveFile(const QString &path, const QString &albumFolderName, QString *errorMessage);

    /// Moves an entire album folder into the trash, used when the album itself is gone.
    QString moveFolder(const QString &path, QString *errorMessage);

    /// Never overwrite something already in the trash — suffix until the name is free.
    static QString availablePath(const QString &folder, const QString &preferredName);

    int itemCount() const;

private:
    QString m_rootPath;
};

} // namespace immichksync
