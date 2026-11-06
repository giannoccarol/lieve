#pragma once

#include <QCollator>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QWidget>

class NpmScripts;
class QFileSystemModel;
class QLabel;
class QStackedWidget;
class QTreeView;

// Hides excluded names, sorts folders first (natural order) and serves the
// VS Code icons, including the "open folder" variant for expanded folders.
class ExplorerProxy : public QSortFilterProxyModel
{
public:
    explicit ExplorerProxy(QObject *parent = nullptr);
    void setExcluded(const QStringList &names);
    void setExpanded(const QModelIndex &proxyIndex, bool expanded);
    void clearExpanded();
    QFileSystemModel *fs() const;
    QVariant data(const QModelIndex &index, int role) const override;

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    QSet<QString> m_excluded;
    QSet<QString> m_expanded;
    QCollator m_collator;
};

class Explorer : public QWidget
{
    Q_OBJECT
public:
    explicit Explorer(QWidget *parent = nullptr);
    void setRoot(const QString &dir);
    void setBranch(const QString &branch);
    void reveal(const QString &path);
    void focusTree();
    // The NPM SCRIPTS section below the tree.
    NpmScripts *npmScripts() const
    {
        return m_npm;
    }

protected:
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

Q_SIGNALS:
    void openFileRequested(const QString &path);
    void openFolderRequested();
    void terminalRequested(const QString &dir);
    void pathRenamed(const QString &oldPath, const QString &newPath);
    void pathRemoved(const QString &path);

private:
    bool tryReveal();
    QString currentPath() const;
    QString targetDir() const;
    void activate(const QModelIndex &index);
    void showMenu(const QPoint &pos);
    void createEntry(bool folder);
    void removeCurrent();
    void collapseAll();

    QFileSystemModel *m_fs = nullptr;
    ExplorerProxy *m_proxy = nullptr;
    QTreeView *m_tree = nullptr;
    QStackedWidget *m_stack = nullptr;
    QWidget *m_headerActions = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_branchIcon = nullptr;
    QLabel *m_branch = nullptr;
    QString m_root;
    QString m_pendingReveal;
    NpmScripts *m_npm = nullptr;
};
