#pragma once

#include <QFrame>
#include <QLabel>
#include <QString>
#include <QWidget>

class QFormLayout;
class QVBoxLayout;

namespace immichksync {

/// Shared chrome so every tab has the same rhythm: a titled group, a form inside it,
/// and an optional explanatory footer underneath.
class SettingsSection : public QWidget {
    Q_OBJECT

public:
    SettingsSection(const QString &title, QWidget *parent = nullptr);

    /// The form to add rows to.
    QFormLayout *form() { return m_form; }
    /// Adds a full-width widget below the form rows.
    void addWidget(QWidget *widget);
    /// Small grey explanatory text under the section.
    void setFooter(const QString &text);

private:
    QFormLayout *m_form;
    QVBoxLayout *m_layout;
    QLabel *m_footer;
};

/// Inline status line used by the connection test, sign-in and certificate imports.
class InlineResult : public QWidget {
    Q_OBJECT

public:
    enum class Kind {
        Success,
        Failure,
        Info,
    };

    explicit InlineResult(QWidget *parent = nullptr);

    void show(Kind kind, const QString &message);
    void clear();

private:
    QLabel *m_icon;
    QLabel *m_text;
};

/// A page that scrolls when the window is too short for its content, so no tab can
/// ever hide a control below the fold.
QWidget *makeScrollablePage(QWidget *content);

} // namespace immichksync
