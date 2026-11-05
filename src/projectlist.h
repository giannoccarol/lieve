#pragma once

#include <QList>
#include <QWidget>

#include "projects.h"

class QLineEdit;
class QListWidget;

// Filterable list of projects (pinned, recent, discovered). Used by the
// chooser shown in empty windows and by the Ctrl+R switcher.
class ProjectList : public QWidget
{
    Q_OBJECT
public:
    explicit ProjectList(QWidget *parent = nullptr);
    void reload();
    void focusFilter();
    void clearFilter();
    void setPlaceholderText(const QString &text);
    // Not preselected even if it is the most recent (the window's own project).
    void setCurrentProject(const QString &path);

Q_SIGNALS:
    // alternate = Ctrl+Enter / Ctrl+click
    void projectChosen(const QString &path, bool alternate);
    void cancelled();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refilter();
    void choose(bool alternate);
    void showMenu(const QPoint &pos);

    QLineEdit *m_filter = nullptr;
    QListWidget *m_list = nullptr;
    QList<Lieve::Project> m_projects;
    QString m_current;
};
