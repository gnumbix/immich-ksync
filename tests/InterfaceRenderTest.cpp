#include "app/AppEnvironment.h"
#include "settings/SettingsWindow.h"

#include <QDir>
#include <QImage>
#include <QPixmap>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>

using namespace immichksync;

namespace {

/// True when every pixel is the same colour — which is what a tab that failed to lay
/// out looks like, and is the bug class this suite exists to catch.
bool isBlank(const QImage &image)
{
    if (image.isNull() || image.width() < 8 || image.height() < 8) {
        return true;
    }
    const QRgb first = image.pixel(0, 0);
    for (int y = 0; y < image.height(); y += 2) {
        for (int x = 0; x < image.width(); x += 2) {
            if (image.pixel(x, y) != first) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

/// Renders each settings tab through a real widget hierarchy and fails if one comes
/// out blank. Cheap, and it catches the whole class of "the tab exists but shows
/// nothing" bugs that no unit test can see.
///
/// `make snapshots` runs this with IMMICHKSYNC_SNAPSHOT_DIR set to also write the PNGs.
class InterfaceRenderTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        // Keep the config and database out of the real home directory entirely.
        QStandardPaths::setTestModeEnabled(true);
        m_directory = std::make_unique<QTemporaryDir>();
        QVERIFY(m_directory->isValid());

        m_environment = std::make_unique<AppEnvironment>(
            m_directory->filePath(QStringLiteral("state.sqlite")));
        QVERIFY2(m_environment->fatalStartupError().isEmpty(),
                 qUtf8Printable(m_environment->fatalStartupError()));

        m_window = std::make_unique<SettingsWindow>(m_environment.get());
        m_window->resize(680, 560);
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
    }

    void cleanupTestCase()
    {
        m_window.reset();
        m_environment.reset();
        m_directory.reset();
    }

    void hasTheFourSpecifiedTabs()
    {
        QTabWidget *tabs = m_window->tabs();
        QCOMPARE(tabs->count(), 4);
        QCOMPARE(tabs->tabText(0), QStringLiteral("Server"));
        QCOMPARE(tabs->tabText(1), QStringLiteral("Folder"));
        QCOMPARE(tabs->tabText(2), QStringLiteral("Albums"));
        QCOMPARE(tabs->tabText(3), QStringLiteral("Advanced"));
    }

    void everyTabRendersSomething_data()
    {
        QTest::addColumn<int>("index");
        QTest::addColumn<QString>("name");
        QTest::newRow("server") << 0 << QStringLiteral("server");
        QTest::newRow("folder") << 1 << QStringLiteral("folder");
        QTest::newRow("albums") << 2 << QStringLiteral("albums");
        QTest::newRow("advanced") << 3 << QStringLiteral("advanced");
    }

    void everyTabRendersSomething()
    {
        QFETCH(int, index);
        QFETCH(QString, name);

        QTabWidget *tabs = m_window->tabs();
        tabs->setCurrentIndex(index);
        QTest::qWait(120);

        const QPixmap pixmap = m_window->grab();
        QVERIFY(!pixmap.isNull());
        const QImage image = pixmap.toImage();

        const QByteArray snapshotDir = qgetenv("IMMICHKSYNC_SNAPSHOT_DIR");
        if (!snapshotDir.isEmpty()) {
            const QString directory = QString::fromLocal8Bit(snapshotDir);
            QDir().mkpath(directory);
            QVERIFY(image.save(QDir(directory).filePath(
                QStringLiteral("settings-%1.png").arg(name))));
        }

        QVERIFY2(!isBlank(image),
                 qUtf8Printable(QStringLiteral("the %1 tab rendered blank").arg(name)));
        QVERIFY(image.width() >= 600);
        QVERIFY(image.height() >= 400);
    }

    /// The window must survive being closed and reopened, because that is what the
    /// tray's Settings item does every time.
    void survivesBeingClosedAndReopened()
    {
        m_window->close();
        QTest::qWait(50);
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
        QVERIFY(!m_window->grab().isNull());
    }

private:
    std::unique_ptr<QTemporaryDir> m_directory;
    std::unique_ptr<AppEnvironment> m_environment;
    std::unique_ptr<SettingsWindow> m_window;
};

QTEST_MAIN(InterfaceRenderTest)
#include "InterfaceRenderTest.moc"
