#pragma once

#include "credentials/CertificateSummary.h"
#include "credentials/ImmichCredentials.h"

#include <QWidget>

#include <optional>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

namespace immichksync {

class AppEnvironment;
class InlineResult;
class SettingsSection;

/// Address, credentials, optional TLS material, and a connection test that names any
/// missing API-key permission before the first sync can fail on one.
class ServerSettingsTab : public QWidget {
    Q_OBJECT

public:
    explicit ServerSettingsTab(AppEnvironment *environment, QWidget *parent = nullptr);

private Q_SLOTS:
    void runConnectionTest();
    void saveApiKey();
    void signIn();
    void signOut();
    void chooseClientCertificate();
    void chooseCertificateAuthority();
    void refreshCertificateRows();
    void refreshCredentialRows();

private:
    void buildServerSection(QVBoxLayout *layout);
    void buildAuthenticationSection(QVBoxLayout *layout);
    void buildClientCertificateSection(QVBoxLayout *layout);
    void buildCertificateAuthoritySection(QVBoxLayout *layout);
    void buildConnectionSection(QVBoxLayout *layout);

    void showProfileResult();
    /// Names exactly which permissions the key lacks, so the fix is a two-minute edit
    /// in Immich rather than a mid-sync 403.
    void showPermissionChecklist(const QList<ImmichPermission> &missing);
    void setInstalledRow(QLabel *label, const std::optional<CertificateSummary> &summary);
    void setBusy(bool busy);

    AppEnvironment *m_environment;

    QLineEdit *m_address = nullptr;
    QLabel *m_apiEndpoint = nullptr;
    QComboBox *m_authMode = nullptr;
    QStackedWidget *m_authStack = nullptr;

    QLineEdit *m_apiKey = nullptr;
    QPushButton *m_saveKey = nullptr;
    QLineEdit *m_email = nullptr;
    QLineEdit *m_password = nullptr;
    QPushButton *m_signIn = nullptr;

    QLabel *m_clientCertificateRow = nullptr;
    QLineEdit *m_certificatePassphrase = nullptr;
    QPushButton *m_removeClientCertificate = nullptr;
    InlineResult *m_clientCertificateResult = nullptr;

    QLabel *m_authorityRow = nullptr;
    QPushButton *m_removeAuthority = nullptr;
    InlineResult *m_authorityResult = nullptr;

    QPushButton *m_testButton = nullptr;
    QPushButton *m_signOutButton = nullptr;
    InlineResult *m_testResult = nullptr;
    QLabel *m_permissionChecklist = nullptr;
    bool m_busy = false;
};

} // namespace immichksync
