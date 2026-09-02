#pragma once

#include "storage/Records.h"

#include <QWidget>

class QLabel;
class QTableWidget;
class QVBoxLayout;

namespace immichksync {

class AppEnvironment;

/// Every album with a per-album on/off switch — and any album the safety gate is
/// holding, with both ways to resolve it.
class AlbumsSettingsTab : public QWidget {
    Q_OBJECT

public:
    explicit AlbumsSettingsTab(AppEnvironment *environment, QWidget *parent = nullptr);

private Q_SLOTS:
    void refresh();

private:
    void rebuildSafetyBanner();
    void rebuildTable();
    void updateFooter();
    QString holdExplanation(const QList<HeldRemoval> &removals) const;

    AppEnvironment *m_environment;
    QWidget *m_bannerContainer = nullptr;
    QVBoxLayout *m_bannerLayout = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_emptyLabel = nullptr;
    QLabel *m_footer = nullptr;
    /// Set while the table is being rebuilt, so programmatic checkbox changes are not
    /// mistaken for the user clicking one.
    bool m_updating = false;
};

} // namespace immichksync
