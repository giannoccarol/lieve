#pragma once

#include "commandpalette.h"

#include <QFrame>
#include <QPointer>
#include <QStringList>

#include <functional>

class HintLineEdit;
class QListWidget;
class QProcess;
namespace KTextEditor
{
class View;
}

// Ctrl+P: fuzzy file finder shown as an overlay at the top of the window.
// Typing "name:42" opens the file at line 42. Like VS Code's, the text picks
// what it lists: ">" the commands (Ctrl+Shift+P), ":" a line to go to in the
// current editor (Ctrl+G).
class QuickOpen : public QFrame
{
    Q_OBJECT
public:
    explicit QuickOpen(QWidget *parent);
    void setRoot(const QString &dir);
    // Opens with `text` already typed: "" files, ">" commands, ":" go to line.
    void popup(const QStringList &recentFiles, const QString &text = QString());
    // The commands listed after ">", gathered each time that list opens.
    void setCommandSource(const std::function<QList<Lieve::Command>()> &source);
    // The editor that ":" moves in.
    void setEditorSource(const std::function<KTextEditor::View *()> &source);

Q_SIGNALS:
    void fileChosen(const QString &path, int line);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class Mode {
        Files,
        Commands,
        Choices, // second list of a command (languages, encodings)
        Line,
    };
    void reloadFiles();
    void refilter();
    void refilterFiles(QString query);
    void refilterCommands(const QString &typed);
    void refilterChoices(const QString &typed);
    void refilterLine(const QString &text);
    void addInfo(const QString &text);
    void accept();
    void runCommand(const Lieve::Command &command);
    void dismiss();
    void place();

    HintLineEdit *m_edit = nullptr;
    QListWidget *m_list = nullptr;
    QProcess *m_proc = nullptr;
    QString m_root;
    QStringList m_files; // relative paths
    QStringList m_filesLower;
    QStringList m_recent; // absolute paths
    bool m_filesStale = true;
    int m_line = -1;
    int m_column = 0;
    Mode m_mode = Mode::Files;
    QPointer<QWidget> m_returnFocus; // focused before opening, focused again on Esc
    std::function<QList<Lieve::Command>()> m_commandSource;
    std::function<KTextEditor::View *()> m_editorSource;
    QList<Lieve::Command> m_commands;
    QStringList m_labels; // of m_commands
    Lieve::Command m_choiceCommand;
    QList<Lieve::CommandChoice> m_choices;
};
