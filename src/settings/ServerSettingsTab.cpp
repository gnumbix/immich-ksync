#include "settings/ServerSettingsTab.h"

#include "app/AppEnvironment.h"
#include "settings/SettingsWidgets.h"

#include <KLocalizedString>

#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace immichksync {

ServerSettingsTab::ServerSettingsTab(AppEnvironment *environment, QWidget *parent)
    : QWidget(parent)
    , m_environment(environment)
{
    auto *layout = new QVBoxLayout(this);
    buildServerSection(layout);
    buildAuthenticationSection(layout);
    buildClientCertificateSection(layout);
    buildCertificateAuthoritySection(layout);
    buildConnectionSection(layout);
    layout->addStretch();

    connect(m_environment,
            &AppEnvironment::certificateStateChanged,
            this,
            &ServerSettingsTab::refreshCertificateRows);
    connect(m_environment,
            &AppEnvironment::credentialStateChanged,
            this,
            &ServerSettingsTab::refreshCredentialRows);

    refreshCertificateRows();
    refreshCredentialRows();
    showProfileResult();
}

void ServerSettingsTab::buildServerSection(QVBoxLayout *layout)
{
    auto *section = new SettingsSection(i18n("Server"), this);
    section->setFooter(i18n("Enter the address you use in a browser. Immich publishes its API "
                            "location at /.well-known/immich, so the /api suffix is discovered "
                            "automatically."));

    m_address = new QLineEdit(m_environment->preferences()->serverAddress(), this);
    m_address->setPlaceholderText(QStringLiteral("https://immich.example.com"));
    connect(m_address, &QLineEdit::editingFinished, this, [this]() {
        m_environment->preferences()->setServerAddress(m_address->text().trimmed());
    });
    section->form()->addRow(i18n("Address:"), m_address);

    m_apiEndpoint = new QLabel(this);
    m_apiEndpoint->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_apiEndpoint->setEnabled(false);
    section->form()->addRow(i18n("API endpoint:"), m_apiEndpoint);

    layout->addWidget(section);
}

void ServerSettingsTab::buildAuthenticationSection(QVBoxLayout *layout)
{
    auto *section = new SettingsSection(i18n("Authentication"), this);

    m_authMode = new QComboBox(this);
    m_authMode->addItem(displayName(ImmichAuthMode::ApiKey), keyFor(ImmichAuthMode::ApiKey));
    m_authMode->addItem(displayName(ImmichAuthMode::Password), keyFor(ImmichAuthMode::Password));
    m_authMode->setCurrentIndex(
        m_environment->preferences()->authMode() == ImmichAuthMode::ApiKey ? 0 : 1);
    section->form()->addRow(i18n("Method:"), m_authMode);

    m_authStack = new QStackedWidget(this);

    // API key
    auto *keyPage = new QWidget(this);
    auto *keyLayout = new QFormLayout(keyPage);
    m_apiKey = new QLineEdit(keyPage);
    m_apiKey->setEchoMode(QLineEdit::Password);
    keyLayout->addRow(i18n("API key:"), m_apiKey);
    auto *keyRow = new QWidget(keyPage);
    auto *keyRowLayout = new QHBoxLayout(keyRow);
    keyRowLayout->setContentsMargins(0, 0, 0, 0);
    m_saveKey = new QPushButton(i18n("Save Key"), keyRow);
    m_saveKey->setEnabled(false);
    keyRowLayout->addWidget(m_saveKey);
    keyRowLayout->addStretch();
    auto *keyHint = new QLabel(i18n("Create one in Immich under Account Settings ▸ API Keys."),
                               keyRow);
    keyHint->setEnabled(false);
    keyHint->setWordWrap(true);
    keyRowLayout->addWidget(keyHint, 1);
    keyLayout->addRow(QString(), keyRow);
    m_authStack->addWidget(keyPage);

    // Email and password
    auto *passwordPage = new QWidget(this);
    auto *passwordLayout = new QFormLayout(passwordPage);
    m_email = new QLineEdit(m_environment->preferences()->accountEmail(), passwordPage);
    m_email->setPlaceholderText(QStringLiteral("you@example.com"));
    passwordLayout->addRow(i18n("Email:"), m_email);
    m_password = new QLineEdit(passwordPage);
    m_password->setEchoMode(QLineEdit::Password);
    passwordLayout->addRow(i18n("Password:"), m_password);
    auto *passwordRow = new QWidget(passwordPage);
    auto *passwordRowLayout = new QHBoxLayout(passwordRow);
    passwordRowLayout->setContentsMargins(0, 0, 0, 0);
    m_signIn = new QPushButton(i18n("Sign In"), passwordRow);
    m_signIn->setEnabled(false);
    passwordRowLayout->addWidget(m_signIn);
    passwordRowLayout->addStretch();
    auto *passwordHint =
        new QLabel(i18n("Your password is exchanged for a session token and never stored."),
                   passwordRow);
    passwordHint->setEnabled(false);
    passwordHint->setWordWrap(true);
    passwordRowLayout->addWidget(passwordHint, 1);
    passwordLayout->addRow(QString(), passwordRow);
    m_authStack->addWidget(passwordPage);

    m_authStack->setCurrentIndex(m_authMode->currentIndex());
    section->addWidget(m_authStack);

    connect(m_authMode, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_authStack->setCurrentIndex(index);
        m_environment->preferences()->setAuthMode(index == 0 ? ImmichAuthMode::ApiKey
                                                             : ImmichAuthMode::Password);
        m_testResult->clear();
        m_permissionChecklist->hide();
        m_environment->reconfigureEngine();
    });
    connect(m_apiKey, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_saveKey->setEnabled(!text.trimmed().isEmpty() && !m_busy);
    });
    connect(m_saveKey, &QPushButton::clicked, this, &ServerSettingsTab::saveApiKey);
    connect(m_signIn, &QPushButton::clicked, this, &ServerSettingsTab::signIn);
    const auto updateSignIn = [this]() {
        m_signIn->setEnabled(!m_email->text().isEmpty() && !m_password->text().isEmpty() && !m_busy);
    };
    connect(m_email, &QLineEdit::textChanged, this, updateSignIn);
    connect(m_password, &QLineEdit::textChanged, this, updateSignIn);

    layout->addWidget(section);
}

void ServerSettingsTab::buildClientCertificateSection(QVBoxLayout *layout)
{
    auto *section = new SettingsSection(i18n("Client Certificate"), this);
    section->setFooter(i18n(
        "Only needed if your server, or the proxy in front of it, asks each client to prove its "
        "identity with a certificate. Choose the PKCS#12 file — usually .p12 or .pfx — that was "
        "issued for this machine. Both the file and its passphrase are kept in your keyring, and "
        "they survive signing out, because without them the server may not be reachable at all."));

    m_clientCertificateRow = new QLabel(this);
    m_clientCertificateRow->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_clientCertificateRow->setTextFormat(Qt::RichText);
    section->form()->addRow(i18n("Installed:"), m_clientCertificateRow);

    m_certificatePassphrase = new QLineEdit(this);
    m_certificatePassphrase->setEchoMode(QLineEdit::Password);
    m_certificatePassphrase->setPlaceholderText(i18n("Passphrase the file was exported with"));
    section->form()->addRow(i18n("Passphrase:"), m_certificatePassphrase);

    auto *row = new QWidget(this);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    auto *choose = new QPushButton(i18n("Choose File…"), row);
    connect(choose, &QPushButton::clicked, this, &ServerSettingsTab::chooseClientCertificate);
    rowLayout->addWidget(choose);
    m_removeClientCertificate = new QPushButton(i18n("Remove"), row);
    connect(m_removeClientCertificate, &QPushButton::clicked, this, [this]() {
        m_environment->removeClientCertificate();
        m_certificatePassphrase->clear();
        m_clientCertificateResult->show(InlineResult::Kind::Info,
                                        i18n("Client certificate removed."));
    });
    rowLayout->addWidget(m_removeClientCertificate);
    rowLayout->addStretch();
    section->addWidget(row);

    m_clientCertificateResult = new InlineResult(this);
    section->addWidget(m_clientCertificateResult);

    layout->addWidget(section);
}

void ServerSettingsTab::buildCertificateAuthoritySection(QVBoxLayout *layout)
{
    auto *section = new SettingsSection(i18n("Private Certificate Authority"), this);
    section->setFooter(i18n(
        "Only needed if your server's certificate was issued by a certificate authority this "
        "system does not already trust — a company one, or one you run yourself. It is added as "
        "an extra anchor for your server's address alone. The hostname and the expiry date are "
        "still checked exactly as they are for any other site, and every certificate the system "
        "already trusts keeps working. This is not a switch that accepts any certificate."));

    m_authorityRow = new QLabel(this);
    m_authorityRow->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_authorityRow->setTextFormat(Qt::RichText);
    section->form()->addRow(i18n("Installed:"), m_authorityRow);

    auto *row = new QWidget(this);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    auto *choose = new QPushButton(i18n("Choose File…"), row);
    connect(choose, &QPushButton::clicked, this, &ServerSettingsTab::chooseCertificateAuthority);
    rowLayout->addWidget(choose);
    m_removeAuthority = new QPushButton(i18n("Remove"), row);
    connect(m_removeAuthority, &QPushButton::clicked, this, [this]() {
        m_environment->removeCertificateAuthority();
        m_authorityResult->show(InlineResult::Kind::Info, i18n("Certificate authority removed."));
    });
    rowLayout->addWidget(m_removeAuthority);
    rowLayout->addStretch();
    section->addWidget(row);

    m_authorityResult = new InlineResult(this);
    section->addWidget(m_authorityResult);

    layout->addWidget(section);
}

void ServerSettingsTab::buildConnectionSection(QVBoxLayout *layout)
{
    auto *section = new SettingsSection(i18n("Connection"), this);

    auto *row = new QWidget(this);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    m_testButton = new QPushButton(i18n("Test Connection"), row);
    connect(m_testButton, &QPushButton::clicked, this, &ServerSettingsTab::runConnectionTest);
    rowLayout->addWidget(m_testButton);
    rowLayout->addStretch();
    m_signOutButton = new QPushButton(i18n("Sign Out"), row);
    connect(m_signOutButton, &QPushButton::clicked, this, &ServerSettingsTab::signOut);
    rowLayout->addWidget(m_signOutButton);
    section->addWidget(row);

    m_testResult = new InlineResult(this);
    section->addWidget(m_testResult);

    m_permissionChecklist = new QLabel(this);
    m_permissionChecklist->setTextFormat(Qt::RichText);
    m_permissionChecklist->setWordWrap(true);
    m_permissionChecklist->hide();
    section->addWidget(m_permissionChecklist);

    layout->addWidget(section);
}

// MARK: - Actions

void ServerSettingsTab::setBusy(bool busy)
{
    m_busy = busy;
    m_testButton->setEnabled(!busy && !m_address->text().trimmed().isEmpty());
    m_saveKey->setEnabled(!busy && !m_apiKey->text().trimmed().isEmpty());
    m_signIn->setEnabled(!busy && !m_email->text().isEmpty() && !m_password->text().isEmpty());
    setCursor(busy ? Qt::WaitCursor : Qt::ArrowCursor);
}

void ServerSettingsTab::runConnectionTest()
{
    // The address the user has typed but not yet committed must be what gets tested.
    m_environment->preferences()->setServerAddress(m_address->text().trimmed());

    setBusy(true);
    const auto result = m_environment->testConnection();
    setBusy(false);

    if (!result.isSuccess()) {
        m_permissionChecklist->hide();
        m_testResult->show(InlineResult::Kind::Failure,
                           result.errorMessage.isEmpty()
                               ? i18n("Could not reach the server.")
                               : result.errorMessage);
        return;
    }
    showProfileResult();
}

void ServerSettingsTab::showProfileResult()
{
    const auto profile = m_environment->status()->serverProfile();
    if (!profile) {
        m_testResult->clear();
        m_permissionChecklist->hide();
        return;
    }

    m_apiEndpoint->setText(profile->apiBaseUrl.toString());
    m_testResult->show(profile->isUsable() ? InlineResult::Kind::Success
                                           : InlineResult::Kind::Failure,
                       i18n("Immich %1 — signed in as %2 (%3)",
                            profile->version.toString(),
                            profile->user.name.isEmpty() ? profile->user.email : profile->user.name,
                            profile->user.email));
    showPermissionChecklist(profile->missingPermissions);
}

void ServerSettingsTab::showPermissionChecklist(const QList<ImmichPermission> &missing)
{
    if (missing.isEmpty()) {
        m_permissionChecklist->hide();
        return;
    }

    QString html = QStringLiteral("<p>%1</p><ul>")
                       .arg(i18np("This API key is missing %1 required permission:",
                                  "This API key is missing %1 required permissions:",
                                  missing.size()));
    for (const ImmichPermission permission : missing) {
        html += QStringLiteral("<li><code>%1</code> — %2</li>")
                    .arg(keyFor(permission).toHtmlEscaped(), purpose(permission).toHtmlEscaped());
    }
    html += QStringLiteral("</ul>");
    m_permissionChecklist->setText(html);
    m_permissionChecklist->show();
}

void ServerSettingsTab::saveApiKey()
{
    m_environment->preferences()->setServerAddress(m_address->text().trimmed());

    setBusy(true);
    const QString error = m_environment->saveApiKey(m_apiKey->text());
    setBusy(false);

    if (!error.isEmpty()) {
        m_testResult->show(InlineResult::Kind::Failure, error);
        return;
    }
    m_apiKey->clear();
    runConnectionTest();
}

void ServerSettingsTab::signIn()
{
    m_environment->preferences()->setServerAddress(m_address->text().trimmed());

    setBusy(true);
    const QString error = m_environment->signIn(m_email->text().trimmed(), m_password->text());
    setBusy(false);

    if (!error.isEmpty()) {
        m_testResult->show(InlineResult::Kind::Failure, error);
        return;
    }
    m_password->clear();
    runConnectionTest();
}

void ServerSettingsTab::signOut()
{
    m_environment->signOut();
    m_apiKey->clear();
    m_password->clear();
    m_testResult->clear();
    m_permissionChecklist->hide();
}

void ServerSettingsTab::chooseClientCertificate()
{
    const QString path = QFileDialog::getOpenFileName(
        this,
        i18n("Choose a Client Certificate"),
        QString(),
        // "All files" is offered too, because the real check is the import itself,
        // which reports precisely what is wrong with the bytes.
        i18n("PKCS#12 certificates (*.p12 *.pfx);;All files (*)"));
    if (path.isEmpty()) {
        // Cancelling is not a result: leave the previous status line exactly as it was.
        return;
    }

    setBusy(true);
    const auto result =
        m_environment->importClientCertificate(path, m_certificatePassphrase->text());
    setBusy(false);

    if (result.succeeded()) {
        m_certificatePassphrase->clear();
        m_clientCertificateResult->show(InlineResult::Kind::Success,
                                        i18n("Client certificate imported."));
    } else {
        m_clientCertificateResult->show(InlineResult::Kind::Failure, result.error.message());
    }
}

void ServerSettingsTab::chooseCertificateAuthority()
{
    const QString path =
        QFileDialog::getOpenFileName(this,
                                     i18n("Choose a Certificate Authority"),
                                     QString(),
                                     i18n("Certificates (*.crt *.pem *.cer *.der);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    setBusy(true);
    const auto result = m_environment->importCertificateAuthority(path);
    setBusy(false);

    if (result.succeeded()) {
        m_authorityResult->show(InlineResult::Kind::Success,
                                i18n("Certificate authority imported."));
    } else {
        m_authorityResult->show(InlineResult::Kind::Failure, result.error.message());
    }
}

void ServerSettingsTab::setInstalledRow(QLabel *label,
                                        const std::optional<CertificateSummary> &summary)
{
    if (!summary) {
        label->setText(i18n("None"));
        return;
    }
    // Names what is installed rather than only that something is: the fingerprint is
    // what lets someone check out of band that this is the certificate they were given.
    label->setText(QStringLiteral("%1<br/><small>%2</small><br/><small><code>%3</code></small>")
                       .arg(summary->commonName.toHtmlEscaped(),
                            summary->validityDescription().toHtmlEscaped(),
                            summary->fingerprint.toHtmlEscaped()));
}

void ServerSettingsTab::refreshCertificateRows()
{
    const auto client = m_environment->certificates()->installedClientCertificate();
    const auto authority = m_environment->certificates()->installedCertificateAuthority();
    setInstalledRow(m_clientCertificateRow, client);
    setInstalledRow(m_authorityRow, authority);
    m_removeClientCertificate->setVisible(client.has_value());
    m_removeAuthority->setVisible(authority.has_value());
}

void ServerSettingsTab::refreshCredentialRows()
{
    m_apiKey->setPlaceholderText(m_environment->hasStoredApiKey()
                                     ? i18n("Stored in your keyring")
                                     : i18n("Paste an API key"));
    m_password->setPlaceholderText(m_environment->hasStoredSessionToken() ? i18n("Signed in")
                                                                         : i18n("Password"));
    m_signOutButton->setVisible(m_environment->hasStoredApiKey()
                                || m_environment->hasStoredSessionToken());

    const QUrl base = m_environment->preferences()->apiBaseUrl();
    m_apiEndpoint->setText(base.isEmpty() ? i18n("Not discovered yet") : base.toString());
}

} // namespace immichksync
