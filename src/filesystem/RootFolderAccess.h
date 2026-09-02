#pragma once

#include <QString>

namespace immichksync {

/// Confirms the sync folder is usable before a cycle infers anything from it.
///
/// This is the guard that keeps an unmounted drive from being read as a mass deletion.
/// macOS needs security-scoped bookmarks here; on Linux a path is a path, so only the
/// validation half of that abstraction survives — and it is the half that mattered.
namespace RootFolderAccess {

enum class Validation {
    Usable,
    Missing,
    NotADirectory,
    NotWritable,
};

Validation validate(const QString &path);
bool isUsable(Validation validation);
/// Empty when usable; otherwise a sentence for the settings window and the tray.
QString message(Validation validation);

/// A warning about *where* the folder is, rather than whether it works.
///
/// Empty when there is nothing to say. These are the Linux counterparts of the macOS
/// build's iCloud and Photos-library checks: places where a sync tool will misbehave
/// through no fault of its own.
QString locationWarning(const QString &path);

} // namespace RootFolderAccess

} // namespace immichksync
