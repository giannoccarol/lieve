#pragma once

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QPointer>
#include <QWidget>

class KXMLGUIClient;
class QKeyEvent;
class QMenu;
class TabView;
namespace KParts
{
class ReadOnlyPart;
}

// Bottom panel with one or more embedded Konsole terminals (konsolepart),
// so the shell, profile, colors and fonts are the user's Konsole ones.
class TerminalPanel : public QWidget
{
    Q_OBJECT
public:
    explicit TerminalPanel(QWidget *parent = nullptr);
    ~TerminalPanel() override;
    void setWorkingDirectory(const QString &dir);
    // Keys the window keeps even while the terminal has focus (Ctrl+J, Ctrl+P, ...).
    void setReservedShortcuts(const QList<QKeySequence> &keys);
    bool hasTerminals() const;
    // `shell` is a path; empty = the default shell ([terminal] shell=).
    void newTerminal(const QString &dir = QString(), const QString &shell = QString());
    void focusCurrent();
    void closeAll();

Q_SIGNALS:
    void hideRequested();
    void lastTerminalClosed();

private Q_SLOTS:
    void overrideShortcut(QKeyEvent *event, bool &override);

private:
    void stripReservedShortcuts(KXMLGUIClient *client);
    void fillShellMenu(QMenu *menu);
    void closeTerminal(int index);

    TabView *m_tabs = nullptr;
    QString m_dir;
    QList<QKeySequence> m_reserved;
    QHash<QWidget *, QPointer<KParts::ReadOnlyPart>> m_parts;
};
