#include "explorer.h"

#include "settings.h"
#include "vscodeicons.h"

#include <QAbstractFileIconProvider>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
// QFileSystemModel asks for icons on a worker thread; we draw our own icons
// in the proxy instead, so this provider does no work at all.
class NullIconProvider : public QAbstractFileIconProvider
{
public:
    QIcon icon(IconType) const override
    {
        return {};
    }
    QIcon icon(const QFileInfo &) const override
    {
        return {};
    }
};

QToolButton *headerButton(const QString &icon, const QString &fallback, const QString &tip, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setIcon(QIcon::fromTheme(icon, QIcon::fromTheme(fallback)));
    button->setToolTip(tip);
    button->setAutoRaise(true);
    button->setIconSize(QSize(16, 16));
    return button;
}
}

ExplorerProxy::ExplorerProxy(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    m_collator.setNumericMode(true);
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);
}

QFileSystemModel *ExplorerProxy::fs() const
{
    return static_cast<QFileSystemModel *>(sourceModel());
}

void ExplorerProxy::setExcluded(const QStringList &names)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
#endif
    m_excluded.clear();
    for (const QString &n : names) {
        if (!n.trimmed().isEmpty()) {
            m_excluded.insert(n.trimmed());
        }
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    endFilterChange();
#else
    invalidateFilter();
#endif
}

void ExplorerProxy::setExpanded(const QModelIndex &proxyIndex, bool expanded)
{
    const QString path = fs()->filePath(mapToSource(proxyIndex));
    if (expanded) {
        m_expanded.insert(path);
    } else {
        m_expanded.remove(path);
    }
    Q_EMIT dataChanged(proxyIndex, proxyIndex, {Qt::DecorationRole});
}

void ExplorerProxy::clearExpanded()
{
    m_expanded.clear();
}

QVariant ExplorerProxy::data(const QModelIndex &index, int role) const
{
    if (role == Qt::DecorationRole && index.column() == 0) {
        const QModelIndex src = mapToSource(index);
        const QString name = fs()->fileName(src);
        if (fs()->isDir(src)) {
            return VsCodeIcons::instance().folderIcon(name, m_expanded.contains(fs()->filePath(src)));
        }
        return VsCodeIcons::instance().fileIcon(name);
    }
    return QSortFilterProxyModel::data(index, role);
}

bool ExplorerProxy::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    const QModelIndex idx = fs()->index(sourceRow, 0, sourceParent);
    const QString name = fs()->fileName(idx);
    // The editor's own recovery files (".name.kate-swp") are not the project's.
    return !m_excluded.contains(name) && !name.endsWith(QLatin1String(".kate-swp"));
}

bool ExplorerProxy::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    const bool leftDir = fs()->isDir(left);
    const bool rightDir = fs()->isDir(right);
    if (leftDir != rightDir) {
        return leftDir;
    }
    return m_collator.compare(fs()->fileName(left), fs()->fileName(right)) < 0;
}

Explorer::Explorer(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Header: FOLDER NAME  ⎇ branch   [new file] [new folder] [collapse]
    auto *header = new QWidget(this);
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(12, 4, 4, 4);
    hl->setSpacing(4);
    m_title = new QLabel(tr("EXPLORER"), header);
    m_title->setObjectName(QStringLiteral("sideTitle"));
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() * 0.85);
    m_title->setFont(titleFont);
    m_branchIcon = new QLabel(header);
    m_branchIcon->setPixmap(QIcon::fromTheme(QStringLiteral("vcs-branch"), QIcon::fromTheme(QStringLiteral("git"))).pixmap(12, 12));
    m_branchIcon->hide();
    m_branch = new QLabel(header);
    QFont branchFont = m_branch->font();
    branchFont.setPointSizeF(branchFont.pointSizeF() * 0.85);
    m_branch->setFont(branchFont);
    m_branch->setForegroundRole(QPalette::PlaceholderText);
    // Long names get clipped instead of widening the side bar.
    m_title->setMinimumWidth(1);
    m_branch->setMinimumWidth(1);
    hl->addWidget(m_title);
    hl->addSpacing(4);
    hl->addWidget(m_branchIcon);
    hl->addWidget(m_branch);
    hl->addStretch(1);

    // Buttons appear only while the mouse is over the explorer, like VS Code.
    m_headerActions = new QWidget(header);
    auto *al = new QHBoxLayout(m_headerActions);
    al->setContentsMargins(0, 0, 0, 0);
    al->setSpacing(0);
    auto *newFile = headerButton(QStringLiteral("document-new"), QStringLiteral("list-add"), tr("New File…"), m_headerActions);
    auto *newFolder = headerButton(QStringLiteral("folder-new"), QStringLiteral("folder"), tr("New Folder…"), m_headerActions);
    auto *collapse = headerButton(QStringLiteral("collapse-all"), QStringLiteral("go-up"), tr("Collapse All"), m_headerActions);
    connect(newFile, &QToolButton::clicked, this, [this] {
        createEntry(false);
    });
    connect(newFolder, &QToolButton::clicked, this, [this] {
        createEntry(true);
    });
    connect(collapse, &QToolButton::clicked, this, &Explorer::collapseAll);
    al->addWidget(newFile);
    al->addWidget(newFolder);
    al->addWidget(collapse);
    hl->addWidget(m_headerActions);
    header->setFixedHeight(header->sizeHint().height());
    m_headerActions->hide();
    layout->addWidget(header);

    m_stack = new QStackedWidget(this);
    layout->addWidget(m_stack, 1);

    // Page 0: no folder open
    auto *empty = new QWidget(m_stack);
    auto *el = new QVBoxLayout(empty);
    el->setContentsMargins(16, 16, 16, 16);
    auto *emptyLabel = new QLabel(tr("No folder open."), empty);
    emptyLabel->setWordWrap(true);
    auto *openButton = new QPushButton(tr("Open Folder…"), empty);
    openButton->setObjectName(QStringLiteral("primaryButton"));
    connect(openButton, &QPushButton::clicked, this, &Explorer::openFolderRequested);
    el->addWidget(emptyLabel);
    el->addWidget(openButton);
    el->addStretch(1);
    m_stack->addWidget(empty);

    // Page 1: the tree
    static NullIconProvider nullIcons;
    m_fs = new QFileSystemModel(this);
    m_fs->setIconProvider(&nullIcons);
    m_fs->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    m_fs->setReadOnly(false); // allows renaming with F2
    m_proxy = new ExplorerProxy(this);
    m_proxy->setSourceModel(m_fs);
    m_proxy->setExcluded(Lieve::config()
                             .value(QStringLiteral("explorer/exclude"),
                                    QStringList{QStringLiteral(".git"),
                                                QStringLiteral(".svn"),
                                                QStringLiteral(".hg"),
                                                QStringLiteral("CVS"),
                                                QStringLiteral(".DS_Store"),
                                                QStringLiteral("Thumbs.db")})
                             .toStringList());
    m_proxy->setDynamicSortFilter(true);
    m_proxy->sort(0, Qt::AscendingOrder);

    m_tree = new QTreeView(m_stack);
    m_tree->setModel(m_proxy);
    m_tree->setHeaderHidden(true);
    for (int c = 1; c < m_proxy->columnCount(); ++c) {
        m_tree->hideColumn(c);
    }
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->setUniformRowHeights(true);
    m_tree->setAnimated(false);
    m_tree->setIndentation(14);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setIconSize(QSize(16, 16));
    m_tree->setExpandsOnDoubleClick(false);
    m_tree->setEditTriggers(QAbstractItemView::EditKeyPressed);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_stack->addWidget(m_tree);

    connect(m_tree, &QTreeView::clicked, this, &Explorer::activate);
    connect(m_tree, &QTreeView::expanded, this, [this](const QModelIndex &i) {
        m_proxy->setExpanded(i, true);
    });
    connect(m_tree, &QTreeView::collapsed, this, [this](const QModelIndex &i) {
        m_proxy->setExpanded(i, false);
    });
    connect(m_tree, &QTreeView::customContextMenuRequested, this, &Explorer::showMenu);
    connect(m_fs, &QFileSystemModel::fileRenamed, this, [this](const QString &dir, const QString &oldName, const QString &newName) {
        Q_EMIT pathRenamed(dir + QLatin1Char('/') + oldName, dir + QLatin1Char('/') + newName);
    });
    connect(m_fs, &QFileSystemModel::directoryLoaded, this, [this] {
        if (!m_pendingReveal.isEmpty()) {
            tryReveal();
        }
    });

    for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
        auto *sc = new QShortcut(QKeySequence(key), m_tree, nullptr, nullptr, Qt::WidgetShortcut);
        connect(sc, &QShortcut::activated, this, [this] {
            activate(m_tree->currentIndex());
        });
    }
    auto *del = new QShortcut(QKeySequence::Delete, m_tree, nullptr, nullptr, Qt::WidgetShortcut);
    connect(del, &QShortcut::activated, this, &Explorer::removeCurrent);
}

void Explorer::setRoot(const QString &dir)
{
    m_root = dir;
    m_pendingReveal.clear();
    m_proxy->clearExpanded();
    if (dir.isEmpty()) {
        m_title->setText(tr("EXPLORER"));
        m_stack->setCurrentIndex(0);
        return;
    }
    const QModelIndex srcRoot = m_fs->setRootPath(dir);
    m_tree->setRootIndex(m_proxy->mapFromSource(srcRoot));
    m_title->setText(QFileInfo(dir).fileName().toUpper());
    m_title->setToolTip(dir);
    m_stack->setCurrentIndex(1);
}

void Explorer::enterEvent(QEnterEvent *event)
{
    m_headerActions->setVisible(!m_root.isEmpty());
    QWidget::enterEvent(event);
}

void Explorer::leaveEvent(QEvent *event)
{
    m_headerActions->hide();
    QWidget::leaveEvent(event);
}

void Explorer::setBranch(const QString &branch)
{
    m_branch->setText(branch);
    m_branchIcon->setVisible(!branch.isEmpty());
}

void Explorer::focusTree()
{
    if (m_stack->currentIndex() == 1) {
        m_tree->setFocus();
    }
}

void Explorer::reveal(const QString &path)
{
    if (m_root.isEmpty() || !path.startsWith(m_root + QLatin1Char('/'))) {
        return;
    }
    m_pendingReveal = path;
    tryReveal();
}

bool Explorer::tryReveal()
{
    const QModelIndex src = m_fs->index(m_pendingReveal);
    const QModelIndex idx = src.isValid() ? m_proxy->mapFromSource(src) : QModelIndex();
    if (!idx.isValid()) {
        return false; // folders still loading: retried on directoryLoaded
    }
    m_pendingReveal.clear();
    for (QModelIndex parent = idx.parent(); parent.isValid() && parent != m_tree->rootIndex(); parent = parent.parent()) {
        m_tree->expand(parent);
    }
    m_tree->setCurrentIndex(idx);
    m_tree->scrollTo(idx);
    return true;
}

QString Explorer::currentPath() const
{
    const QModelIndex idx = m_tree->currentIndex();
    return idx.isValid() ? m_fs->filePath(m_proxy->mapToSource(idx)) : QString();
}

QString Explorer::targetDir() const
{
    const QModelIndex idx = m_tree->currentIndex();
    if (!idx.isValid()) {
        return m_root;
    }
    const QModelIndex src = m_proxy->mapToSource(idx);
    return m_fs->isDir(src) ? m_fs->filePath(src) : QFileInfo(m_fs->filePath(src)).absolutePath();
}

void Explorer::activate(const QModelIndex &index)
{
    if (!index.isValid()) {
        return;
    }
    const QModelIndex src = m_proxy->mapToSource(index);
    if (m_fs->isDir(src)) {
        m_tree->setExpanded(index, !m_tree->isExpanded(index));
    } else {
        Q_EMIT openFileRequested(m_fs->filePath(src));
    }
}

void Explorer::collapseAll()
{
    m_tree->collapseAll();
    m_proxy->clearExpanded();
    m_tree->viewport()->update();
}

void Explorer::showMenu(const QPoint &pos)
{
    if (m_root.isEmpty()) {
        return;
    }
    const QModelIndex idx = m_tree->indexAt(pos);
    if (idx.isValid()) {
        m_tree->setCurrentIndex(idx);
    } else {
        m_tree->setCurrentIndex(QModelIndex());
        m_tree->clearSelection();
    }

    QMenu menu(this);
    menu.addAction(QIcon::fromTheme(QStringLiteral("document-new")), tr("New File…"), this, [this] {
        createEntry(false);
    });
    menu.addAction(QIcon::fromTheme(QStringLiteral("folder-new")), tr("New Folder…"), this, [this] {
        createEntry(true);
    });
    if (idx.isValid()) {
        const QString path = currentPath();
        menu.addSeparator();
        QAction *rename = menu.addAction(QIcon::fromTheme(QStringLiteral("edit-rename")), tr("Rename…"), this, [this, idx] {
            m_tree->edit(idx);
        });
        rename->setShortcut(Qt::Key_F2);
        QAction *trash = menu.addAction(QIcon::fromTheme(QStringLiteral("user-trash")), tr("Move to Trash"), this, &Explorer::removeCurrent);
        trash->setShortcut(QKeySequence::Delete);
        menu.addSeparator();
        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy-path"), QIcon::fromTheme(QStringLiteral("edit-copy"))),
                       tr("Copy Path"),
                       this,
                       [path] {
                           QApplication::clipboard()->setText(path);
                       });
        menu.addAction(tr("Copy Relative Path"), this, [this, path] {
            QApplication::clipboard()->setText(QDir(m_root).relativeFilePath(path));
        });
    }
    menu.addSeparator();
    menu.addAction(QIcon::fromTheme(QStringLiteral("utilities-terminal")), tr("Open in Integrated Terminal"), this, [this] {
        Q_EMIT terminalRequested(targetDir());
    });
    menu.addAction(QIcon::fromTheme(QStringLiteral("system-file-manager")), tr("Reveal in File Manager"), this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(targetDir()));
    });
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void Explorer::createEntry(bool folder)
{
    if (m_root.isEmpty()) {
        return;
    }
    const QString dir = targetDir();
    bool ok = false;
    const QString name = QInputDialog::getText(this,
                                               folder ? tr("New Folder") : tr("New File"),
                                               tr("Name (subfolders allowed, e.g. utils/date.ts):"),
                                               QLineEdit::Normal,
                                               QString(),
                                               &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    const QString path = QDir::cleanPath(dir + QLatin1Char('/') + name);
    if (QFileInfo::exists(path)) {
        QMessageBox::warning(this, QStringLiteral("Lieve"), tr("“%1” already exists.").arg(name));
        return;
    }
    if (folder) {
        if (!QDir().mkpath(path)) {
            QMessageBox::warning(this, QStringLiteral("Lieve"), tr("Could not create the folder:\n%1").arg(path));
            return;
        }
    } else {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) {
            QMessageBox::warning(this, QStringLiteral("Lieve"), tr("Could not create the file:\n%1").arg(path));
            return;
        }
        f.close();
        Q_EMIT openFileRequested(path);
    }
    reveal(path);
}

void Explorer::removeCurrent()
{
    const QString path = currentPath();
    if (path.isEmpty() || path == m_root) {
        return;
    }
    if (!QFile::moveToTrash(path)) {
        QMessageBox::warning(this, QStringLiteral("Lieve"), tr("Could not move to Trash:\n%1").arg(path));
        return;
    }
    Q_EMIT pathRemoved(path);
}
