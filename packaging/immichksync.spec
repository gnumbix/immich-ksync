# ImmichKSync — Fedora/openSUSE package.
#
# There is no bundled dependency here: everything comes from Qt 6, KDE Frameworks 6 or
# the C library, which is why the BuildRequires list is short and the Requires list is
# almost empty.

Name:           immichksync
Version:        1.0.0
Release:        1%{?dist}
Summary:        Your Immich albums, as ordinary folders on your desktop

License:        AGPL-3.0-or-later
URL:            https://github.com/GnumBix/immich-ksync
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.23
BuildRequires:  ninja-build
BuildRequires:  gcc-c++
BuildRequires:  extra-cmake-modules
BuildRequires:  cmake(Qt6Core)
BuildRequires:  cmake(Qt6Gui)
BuildRequires:  cmake(Qt6Widgets)
BuildRequires:  cmake(Qt6Network)
BuildRequires:  cmake(Qt6DBus)
BuildRequires:  cmake(Qt6Concurrent)
BuildRequires:  cmake(Qt6Test)
BuildRequires:  cmake(KF6CoreAddons)
BuildRequires:  cmake(KF6Config)
BuildRequires:  cmake(KF6ConfigWidgets)
BuildRequires:  cmake(KF6I18n)
BuildRequires:  cmake(KF6Notifications)
BuildRequires:  cmake(KF6StatusNotifierItem)
BuildRequires:  cmake(KF6DBusAddons)
BuildRequires:  cmake(KF6KIO)
BuildRequires:  cmake(KF6WindowSystem)
BuildRequires:  cmake(KF6IconThemes)
BuildRequires:  sqlite-devel

# A secret service is needed to store credentials at all. On Plasma that is ksecretd
# or kwalletd6; the Recommends keeps a headless install from being blocked by it.
Recommends:     (kf6-kwallet or gnome-keyring)

%description
A native KDE system-tray agent that keeps a folder on your computer and your Immich
albums in continuous two-way agreement.

Every album you own becomes a real folder full of real files. Dolphin, digiKam, rsync
and your backup tool all see them, because they are just files. The app renders
nothing — no thumbnails, no gallery, no previews. It is a sync daemon with a menu and
a settings window.

Nothing is ever destroyed locally, and no asset is ever removed from your Immich
library. The strongest action in either direction is "remove from album" or "move to a
trash folder you control".

%prep
%autosetup -n %{name}-%{version}

%build
%cmake_kf6
%cmake_build

%install
%cmake_install

%check
# The hermetic suites only; the live-server and TLS suites are opt-in and skip
# themselves without their environment variables.
export QT_QPA_PLATFORM=offscreen
%ctest --exclude-regex 'LiveServerTest|TlsLiveTest'

%files
%license LICENSE
%doc README.md
%{_bindir}/immichksync
%{_datadir}/applications/com.gnumbix.immichksync.desktop
%{_datadir}/knotifications6/immichksync.notifyrc
%{_metainfodir}/com.gnumbix.immichksync.metainfo.xml
%{_datadir}/icons/hicolor/*/apps/com.gnumbix.immichksync.*

%changelog
* Mon Aug 31 2026 GnumBix <gnumbix@gmail.com> - 1.0.0-1
- First release.
