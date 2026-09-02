#pragma once

#include <QWidget>

class QPlainTextEdit;
class QTimer;

namespace immichksync {

/// A live view of the last few hundred log lines, so a problem can be diagnosed
/// without leaving the app.
class LogTailWidget : public QWidget {
    Q_OBJECT

public:
    explicit LogTailWidget(QWidget *parent = nullptr);

protected:
    /// Polling only while visible: the log tab is rarely open, and a timer running
    /// behind a closed window is pure waste in a process meant to sit idle all day.
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private Q_SLOTS:
    void refresh();

private:
    QPlainTextEdit *m_view;
    QTimer *m_timer;
    quint64 m_lastSeenId = 0;
    bool m_primed = false;
};

} // namespace immichksync
