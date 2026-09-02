#include "settings/AlbumsSettingsTab.h"

#include "app/AppEnvironment.h"
#include "filesystem/AlbumFolderLayout.h"

#include <KLocalizedString>

#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace immichksync {

namespace {

enum Column {
    SyncColumn = 0,
    AlbumColumn,
    FolderColumn,
    ItemsColumn,
    LastSyncedColumn,
    ColumnCount,
};

QString relativeTime(const QDateTime &when)
{
    if (!when.isValid()) {
        return QStringLiteral("—");
    }
    const qint64 seconds = when.secsTo(QDateTime::currentDateTimeUtc());
    if (seconds < 60) {
        return i18n("just now");
    }
    if (seconds < 3600) {
        return i18np("%1 minute ago", "%1 minutes ago", seconds / 60);
    }
    if (seconds < 86400) {
        return i18np("%1 hour ago", "%1 hours ago", seconds / 3600);
    }
    return i18np("%1 day ago", "%1 days ago", seconds / 86400);
}

} // namespace

AlbumsSettingsTab::AlbumsSettingsTab(AppEnvironment *environment, QWidget *parent)
    : QWidget(parent)
    , m_environment(environment)
{
    auto *layout = new QVBoxLayout(this);

    m_bannerContainer = new QWidget(this);
    m_bannerLayout = new QVBoxLayout(m_bannerContainer);
    m_bannerLayout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_bannerContainer);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(ColumnCount);
    m_table->setHorizontalHeaderLabels(
        {i18n("Sync"), i18n("Album"), i18n("Folder"), i18n("Items"), i18n("Last synced")});
    m_table->verticalHeader()->hide();
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->horizontalHeader()->setSectionResizeMode(AlbumColumn, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(FolderColumn, QHeaderView::Stretch);
    layout->addWidget(m_table, 1);

    m_emptyLabel = new QLabel(
        i18n("Albums appear here after the first sync. Create an album in Immich, or make a "
             "folder inside your sync folder to create one."),
        this);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->setEnabled(false);
    layout->addWidget(m_emptyLabel);

    auto *footerRow = new QWidget(this);
    auto *footerLayout = new QHBoxLayout(footerRow);
    footerLayout->setContentsMargins(0, 0, 0, 0);
    m_footer = new QLabel(footerRow);
    m_footer->setEnabled(false);
    footerLayout->addWidget(m_footer, 1);

    auto *syncNow = new QPushButton(i18n("Sync Now"), footerRow);
    connect(syncNow, &QPushButton::clicked, this, [this]() { m_environment->syncNow(); });
    footerLayout->addWidget(syncNow);
    auto *refreshButton = new QPushButton(i18n("Refresh"), footerRow);
    connect(refreshButton, &QPushButton::clicked, this, [this]() { m_environment->refreshAlbums(); });
    footerLayout->addWidget(refreshButton);
    layout->addWidget(footerRow);

    connect(m_environment, &AppEnvironment::albumsChanged, this, &AlbumsSettingsTab::refresh);
    refresh();
}

void AlbumsSettingsTab::refresh()
{
    rebuildSafetyBanner();
    rebuildTable();
    updateFooter();
}

void AlbumsSettingsTab::rebuildSafetyBanner()
{
    while (QLayoutItem *item = m_bannerLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    QSet<QString> heldIds;
    for (const HeldRemoval &removal : m_environment->heldRemovals()) {
        heldIds.insert(removal.albumId);
    }
    m_bannerContainer->setVisible(!heldIds.isEmpty());
    if (heldIds.isEmpty()) {
        return;
    }

    for (const AlbumRecord &album : m_environment->albums()) {
        if (!heldIds.contains(album.albumId)) {
            continue;
        }
        const QList<HeldRemoval> removals = m_environment->heldRemovalsFor(album.albumId);
        int restorable = 0;
        for (const HeldRemoval &removal : removals) {
            if (removal.direction == HeldRemoval::Direction::RemoveFromAlbum) {
                ++restorable;
            }
        }

        auto *banner = new QFrame(m_bannerContainer);
        banner->setFrameShape(QFrame::StyledPanel);
        // An explicit ground rather than a tint over whatever is behind: this is the
        // one place in the app where being noticed matters more than blending in.
        banner->setStyleSheet(QStringLiteral(
            "QFrame { border: 1px solid palette(highlight); border-radius: 6px; "
            "background: palette(alternate-base); }"));

        auto *bannerLayout = new QVBoxLayout(banner);
        auto *title = new QLabel(i18n("“%1” is on hold", album.albumName), banner);
        QFont titleFont = title->font();
        titleFont.setBold(true);
        title->setFont(titleFont);
        bannerLayout->addWidget(title);

        auto *explanation = new QLabel(holdExplanation(removals), banner);
        explanation->setWordWrap(true);
        bannerLayout->addWidget(explanation);

        auto *buttons = new QWidget(banner);
        auto *buttonLayout = new QHBoxLayout(buttons);
        buttonLayout->setContentsMargins(0, 0, 0, 0);

        auto *apply = new QPushButton(i18n("Apply Removals"), buttons);
        const QString albumId = album.albumId;
        connect(apply, &QPushButton::clicked, this, [this, albumId]() {
            m_environment->applyHeldRemovals(albumId);
        });
        buttonLayout->addWidget(apply);

        auto *restore = new QPushButton(
            i18np("Restore %1 File from Immich", "Restore %1 Files from Immich", restorable),
            buttons);
        restore->setEnabled(restorable > 0);
        connect(restore, &QPushButton::clicked, this, [this, albumId]() {
            m_environment->restoreHeldRemovals(albumId);
        });
        buttonLayout->addWidget(restore);
        buttonLayout->addStretch();

        bannerLayout->addWidget(buttons);
        m_bannerLayout->addWidget(banner);
    }
}

QString AlbumsSettingsTab::holdExplanation(const QList<HeldRemoval> &removals) const
{
    int fromAlbum = 0;
    int toTrash = 0;
    for (const HeldRemoval &removal : removals) {
        if (removal.direction == HeldRemoval::Direction::RemoveFromAlbum) {
            ++fromAlbum;
        } else {
            ++toTrash;
        }
    }

    QStringList parts;
    if (fromAlbum > 0) {
        parts << i18np("%1 file disappeared locally, which would remove that asset from the album",
                       "%1 files disappeared locally, which would remove those assets from the "
                       "album",
                       fromAlbum);
    }
    if (toTrash > 0) {
        parts << i18np("%1 asset left the album on the server, which would move the local file to "
                       "%2",
                       "%1 assets left the album on the server, which would move the local files "
                       "to %2",
                       toTrash,
                       QString::fromLatin1(AlbumFolderLayout::kTrashFolderName));
    }
    return i18n("%1. That is more than the safety threshold allows without asking, so nothing was "
                "changed.",
                parts.join(i18n(", and ")));
}

void AlbumsSettingsTab::rebuildTable()
{
    m_updating = true;

    const QList<AlbumRecord> albums = m_environment->albums();
    m_table->setRowCount(static_cast<int>(albums.size()));
    m_table->setVisible(!albums.isEmpty());
    m_emptyLabel->setVisible(albums.isEmpty());

    const QLocale locale;
    for (int row = 0; row < albums.size(); ++row) {
        const AlbumRecord &album = albums.at(row);

        auto *sync = new QTableWidgetItem();
        sync->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        sync->setCheckState(album.isExcluded ? Qt::Unchecked : Qt::Checked);
        sync->setData(Qt::UserRole, album.albumId);
        m_table->setItem(row, SyncColumn, sync);

        m_table->setItem(row, AlbumColumn, new QTableWidgetItem(album.albumName));
        m_table->setItem(row, FolderColumn, new QTableWidgetItem(album.folderName));

        auto *items = new QTableWidgetItem(locale.toString(album.remoteAssetCount));
        items->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_table->setItem(row, ItemsColumn, items);

        m_table->setItem(row, LastSyncedColumn, new QTableWidgetItem(relativeTime(album.lastSyncedAt)));
    }
    m_table->resizeColumnToContents(SyncColumn);
    m_table->resizeColumnToContents(ItemsColumn);
    m_table->resizeColumnToContents(LastSyncedColumn);

    m_updating = false;

    // Reconnected each rebuild because the items themselves are recreated.
    disconnect(m_table, &QTableWidget::itemChanged, this, nullptr);
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_updating || item->column() != SyncColumn) {
            return;
        }
        const QString albumId = item->data(Qt::UserRole).toString();
        m_environment->setAlbumExcluded(item->checkState() != Qt::Checked, albumId);
    });
}

void AlbumsSettingsTab::updateFooter()
{
    const SyncStore::Statistics statistics = m_environment->status()->statistics();
    const QLocale locale;

    QString text = i18n("%1 item(s), %2 synced",
                        locale.toString(statistics.syncedAssetCount),
                        locale.formattedDataSize(statistics.syncedByteCount));
    if (statistics.failureCount > 0) {
        text += i18n(" · %1 item(s) retrying", statistics.failureCount);
    }
    m_footer->setText(text);
}

} // namespace immichksync
