#include "settings/FolderSettingsTab.h"

#include "app/AppEnvironment.h"
#include "filesystem/AlbumFolderLayout.h"
#include "filesystem/RootFolderAccess.h"
#include "settings/SettingsWidgets.h"

#include <KLocalizedString>

#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QStorageInfo>
#include <QVBoxLayout>

namespace immichksync {

FolderSettingsTab::FolderSettingsTab(AppEnvironment *environment, QWidget *parent)
    : QWidget(parent)
    , m_environment(environment)
{
    auto *layout = new QVBoxLayout(this);

    auto *folderSection = new SettingsSection(i18n("Sync Folder"), this);
    folderSection->setFooter(i18n("Every album you own becomes a folder here. Files you add to an "
                                  "album folder are uploaded and joined to that album; assets "
                                  "added to the album elsewhere are downloaded into it."));

    m_location = new QLabel(this);
    m_location->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_location->setWordWrap(true);
    folderSection->form()->addRow(i18n("Location:"), m_location);

    m_capacity = new QLabel(this);
    m_capacity->setEnabled(false);
    folderSection->form()->addRow(i18n("Available space:"), m_capacity);

    auto *row = new QWidget(this);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    auto *choose = new QPushButton(i18n("Choose…"), row);
    connect(choose, &QPushButton::clicked, this, &FolderSettingsTab::chooseFolder);
    rowLayout->addWidget(choose);
    m_open = new QPushButton(i18n("Open Folder"), row);
    connect(m_open, &QPushButton::clicked, this, [this]() { m_environment->openSyncFolder(); });
    rowLayout->addWidget(m_open);
    rowLayout->addStretch();
    folderSection->addWidget(row);

    m_warning = new InlineResult(this);
    folderSection->addWidget(m_warning);
    layout->addWidget(folderSection);

    auto *layoutSection = new SettingsSection(i18n("How the folder is laid out"), this);
    layoutSection->setFooter(
        i18n("Immich albums are flat, so only files placed directly inside an album folder are "
             "synced; nested folders are left alone. A file you delete locally is removed from "
             "the album but stays in your Immich library, and an asset removed from an album on "
             "the server has its local file moved to %1 rather than deleted.",
             QString::fromLatin1(AlbumFolderLayout::kTrashFolderName)));
    layoutSection->addWidget(buildLayoutDiagram());
    layout->addWidget(layoutSection);

    layout->addStretch();
    refresh();

    connect(m_environment->preferences(), &Preferences::changed, this, &FolderSettingsTab::refresh);
}

QWidget *FolderSettingsTab::buildLayoutDiagram()
{
    auto *label = new QLabel(this);
    label->setTextFormat(Qt::RichText);

    const QString trash = QString::fromLatin1(AlbumFolderLayout::kTrashFolderName);
    const QString staging = QString::fromLatin1(AlbumFolderLayout::kStagingFolderName);
    const QString marker = QString::fromLatin1(AlbumFolderLayout::kMarkerFilename);

    const QString diagram =
        QStringLiteral(
            "<pre style='margin:0'>"
            "📁 Holiday 2024\n"
            "   📄 %1        <i>%2</i>\n"
            "   🖼  IMG_0001.HEIC\n"
            "   🖼  IMG_0002.HEIC\n"
            "📁 %3        <i>%4</i>\n"
            "📁 %5      <i>%6</i>"
            "</pre>")
            .arg(marker.toHtmlEscaped(),
                 i18n("which album this folder mirrors").toHtmlEscaped(),
                 trash.toHtmlEscaped(),
                 i18n("files removed from an album, never deleted").toHtmlEscaped(),
                 staging.toHtmlEscaped(),
                 i18n("transfers in progress").toHtmlEscaped());
    label->setText(diagram);
    return label;
}

void FolderSettingsTab::chooseFolder()
{
    const QString current = m_environment->preferences()->rootFolder();
    const QString path = QFileDialog::getExistingDirectory(
        this,
        i18n("Choose Sync Folder"),
        current.isEmpty() ? QDir::homePath() : current,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (path.isEmpty()) {
        return;
    }
    m_environment->setRootFolder(path);
    refresh();
}

void FolderSettingsTab::refresh()
{
    const QString root = m_environment->preferences()->rootFolder();

    m_location->setText(root.isEmpty() ? i18n("Not chosen") : root);
    m_open->setEnabled(!root.isEmpty() && QDir(root).exists());

    if (root.isEmpty()) {
        m_capacity->setText(QStringLiteral("—"));
        m_warning->clear();
        return;
    }

    const QStorageInfo storage(root);
    m_capacity->setText(storage.isValid()
                            ? QLocale().formattedDataSize(storage.bytesAvailable())
                            : QStringLiteral("—"));

    const auto validation = RootFolderAccess::validate(root);
    if (!RootFolderAccess::isUsable(validation)) {
        m_warning->show(InlineResult::Kind::Failure, RootFolderAccess::message(validation));
        return;
    }
    const QString locationWarning = RootFolderAccess::locationWarning(root);
    if (!locationWarning.isEmpty()) {
        m_warning->show(InlineResult::Kind::Failure, locationWarning);
        return;
    }
    m_warning->clear();
}

} // namespace immichksync
