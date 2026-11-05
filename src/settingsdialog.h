#pragma once

#include <QDialog>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <deque>
#include <functional>

class MainWindow;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QScrollArea;
class QVBoxLayout;

// The Settings window (Ctrl+,), like VS Code's: a search box, categories on
// the left and every lieve.conf key with a title, a description and its
// control. Each change is written at once with setConfigValue(), so the
// comments in the file stay; settings that need a restart are marked.
class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    // Shows the window (one for the whole application, built on first use)
    // over `owner`. `filter`: a category ("editor") or text to search for.
    static SettingsDialog *showFor(MainWindow *owner, const QString &filter = QString());

protected:
    void showEvent(QShowEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class Apply {
        Now, // read when used (on save, at the next terminal, ...)
        Editors, // re-applied to the open editors
        Restart,
    };
    struct Category {
        QString id;
        QLabel *header = nullptr;
        int item = -1; // row in the list on the left
    };
    struct Setting {
        QString section;
        QString key;
        QString fallback; // the value used when the key is missing
        QString comment; // written above the key when it is added to the file
        Apply apply = Apply::Now;
        int category = 0;
        QWidget *row = nullptr;
        QWidget *control = nullptr;
        QLabel *restartNote = nullptr;
        QString haystack; // what the search box looks in (lower case)
        std::function<void(const QString &)> show; // value -> control
    };

    explicit SettingsDialog(MainWindow *owner);
    void build();
    void setOwner(MainWindow *owner);
    void applyFilter(const QString &filter);

    // Building blocks: a category, then rows of settings in it.
    void addCategory(const QString &id, const QString &title);
    Setting &addRow(const QString &path, const QString &fallback, Apply apply, const QString &title, const QString &description, QWidget *control,
                    const QStringList &values = {});
    void addCheck(const QString &path, bool fallback, Apply apply, const QString &title, const QString &description);
    void addChoice(const QString &path,
                   const QString &fallback,
                   Apply apply,
                   const QString &title,
                   const QString &description,
                   const QList<QPair<QString, QString>> &choices,
                   bool listValues = true);
    void addAutoOnOff(const QString &path, Apply apply, const QString &title, const QString &description);
    void addNumber(const QString &path, int fallback, int minimum, int maximum, const QString &zeroText, Apply apply, const QString &title,
                   const QString &description);
    QLineEdit *addText(const QString &path, Apply apply, const QString &title, const QString &description, const std::function<QString()> &placeholder);
    void addAccent();

    QString value(const Setting &s) const;
    void write(Setting &s, const QString &value);
    void refresh(Setting &s);
    void reload();
    void updateRestartBar();
    void scrollToCategory(int index);
    void syncCategory();
    void focusFirstControl();
    void restartLieve();
    void openConfigFile();

    QPointer<MainWindow> m_owner;
    QLineEdit *m_search = nullptr;
    QLabel *m_found = nullptr;
    QListWidget *m_categoryList = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_content = nullptr;
    QVBoxLayout *m_rows = nullptr;
    QLabel *m_nothing = nullptr;
    QFrame *m_restartBar = nullptr;
    QList<Category> m_categories;
    std::deque<Setting> m_settings; // grows without moving the rows
    bool m_scrolling = false; // scrolling to a category: keep it selected
};
