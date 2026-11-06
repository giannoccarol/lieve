#pragma once

#include "lsp.h"

#include <QSet>
#include <QWidget>

#include <functional>

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

// The Problems panel, like VS Code's: what the language servers report for
// the open files, by file, errors first. A tab of the bottom panel, made
// the first time it is asked for; the list is only rebuilt while it is on
// screen.
class ProblemsPanel : public QWidget
{
    Q_OBJECT
public:
    using Source = std::function<QList<Lieve::LspManager::Problem>()>;
    // `problems` gives the current problems, `root` the project folder.
    ProblemsPanel(const Source &problems, const std::function<QString()> &root, QWidget *parent = nullptr);
    // The problems changed: the list follows now if it is shown, else when it is.
    void refresh();
    // The icon of its tab.
    static QIcon tabIcon();

Q_SIGNALS:
    void problemActivated(const QString &path, int line, int column);

protected:
    void showEvent(QShowEvent *event) override;

private:
    void rebuild();
    void activate(QTreeWidgetItem *item);

    Source m_problems;
    std::function<QString()> m_root;
    QTreeWidget *m_tree = nullptr;
    QLabel *m_empty = nullptr;
    QSet<QString> m_collapsed; // files folded by the user
    bool m_stale = true;
};
