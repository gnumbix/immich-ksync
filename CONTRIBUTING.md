# Contributing to ImmichKSync

Thank you for wanting to help.

## The one rule

**Anything that can remove a file or an album entry needs a test.**

This app runs unattended over someone's photo library. The worst bug it can have is not
a crash — it is quietly removing things. Every code path that ends in a deletion, a
trashing, or an album removal must be covered by a test that would fail if the path
fired when it should not have.

`src/sync/SyncPlanner.cpp` is where those decisions are made, and it is a pure function
over three snapshots precisely so they can all be tested without a server or a disk.

## Getting set up

```bash
sudo dnf install gcc-c++ cmake ninja-build extra-cmake-modules \
  qt6-qtbase-devel kf6-kcoreaddons-devel kf6-kconfig-devel kf6-kconfigwidgets-devel \
  kf6-ki18n-devel kf6-knotifications-devel kf6-kstatusnotifieritem-devel \
  kf6-kdbusaddons-devel kf6-kio-devel kf6-kwindowsystem-devel kf6-kiconthemes-devel \
  sqlite-devel

make test
```

`make help` lists everything else.

## Running the tests

| Command | Needs |
|---|---|
| `make test` | nothing external |
| `make test-live IMMICH_TEST_SERVER=… IMMICH_TEST_API_KEY=…` | a real Immich server |
| `./tools/run-tls-test-server.sh & make test-tls` | `openssl` |

The live suite creates its own albums, excludes every other album so your real library is
never touched, and deletes what it created. It is still worth pointing at a test server
rather than the one holding your photographs.

## Style

- Match the surrounding code. It is fairly consistent.
- Comments explain **why**, not what. If a line needs a comment to say what it does, the
  line usually wants rewriting instead.
- No third-party dependencies. Everything comes from Qt 6, KDE Frameworks 6, SQLite or
  the C library, and that is a feature — it is why the app builds in a minute and
  packages without vendoring.
- User-facing strings go through `i18n()`.
- Errors are values, not exceptions. The build is compiled with `-fno-exceptions`.

## Security

There is one rule here too: **there is no code path that accepts a TLS certificate the
system would have rejected.** The private-CA feature *adds* an anchor for one configured
host; it never replaces the system anchors, never disables hostname or expiry checking,
and `ignoreSslErrors()` appears nowhere in the codebase. `TlsCertificateStoreTest` and
`TlsLiveTest` both exist to keep that true.

Report vulnerabilities through [SECURITY.md](SECURITY.md), not public issues.
