#include "npmscripts.h"

#include "delegates.h"
#include "quickopen.h"
#include "roundedrows.h"
#include "settings.h"
#include "theme.h"
#include "vscodeicons.h"

#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QCryptographicHash>
#include <QFileSystemWatcher>
#include <QHeaderView>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QSplitter>
#include <QStandardItemModel>
#include <QToolTip>
#include <QTreeView>

#include <algorithm>

using namespace Lieve;

namespace
{
// Also PathRole (folder of the package.json), DirRole (dim text), LineRole
// and ColumnRole (where the script is in package.json)
constexpr int ScriptRole = Qt::UserRole + 60; // script name; empty on package rows
constexpr int kButton = 20; // the run button, on the row under the mouse
constexpr int kRescanDelay = 300; // ms after a package.json changes

// The window's per-project group in state.ini (see sessionKey() in mainwindow.cpp).
QString collapsedKey(const QString &root)
{
    return QStringLiteral("sessions/") + QString::fromLatin1(QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Sha1).toHex().left(16))
        + QStringLiteral("/npmScriptsCollapsed");
}

bool isScript(const QModelIndex &index)
{
    return !index.data(ScriptRole).toString().isEmpty();
}

// Script rows: [icon] name  dim command ............ [▶]
// Package rows (several package.json files): [npm] package.json  dim folder
class ScriptDelegate : public FileItemDelegate
{
public:
    using FileItemDelegate::FileItemDelegate;
    std::function<void(const QModelIndex &)> onRun;
    QIcon runIcon;

    static QRect buttonRect(const QStyleOptionViewItem &option)
    {
        return QRect(option.rect.right() - 6 - kButton + 1, option.rect.center().y() - kButton / 2 + 1, kButton, kButton);
    }

    // As tall as the rows of the file tree above.
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        return QStyledItemDelegate::sizeHint(option, index);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (!isScript(index)) {
            FileItemDelegate::paint(p, option, index);
            return;
        }
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QString name = opt.text;
        opt.text.clear();
        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, p, widget); // background, icon

        const bool selected = opt.state & QStyle::State_Selected;
        const QPalette::ColorGroup group = (opt.state & QStyle::State_Active) ? QPalette::Active : QPalette::Inactive;
        const QColor fg = opt.palette.color(group, selected && !Theme::styled() ? QPalette::HighlightedText : QPalette::Text);
        QColor dim = fg;
        dim.setAlphaF(0.55f);

        QRect r = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget).adjusted(2, 0, -4, 0);
        if (opt.state & QStyle::State_MouseOver) {
            const QRect button = buttonRect(opt);
            runIcon.paint(p, button.adjusted(2, 2, -2, -2));
            r.setRight(button.left() - 4);
        }
        const QFontMetrics fm(opt.font);
        const int baseline = r.top() + (r.height() - fm.height()) / 2 + fm.ascent();
        p->save();
        p->setFont(opt.font);
        p->setClipRect(r);
        p->setPen(fg);
        const QString shown = fm.elidedText(name, Qt::ElideRight, r.width());
        p->drawText(r.left(), baseline, shown);
        const int x = r.left() + fm.horizontalAdvance(shown) + 8;
        const QString command = index.data(DirRole).toString();
        if (!command.isEmpty() && x < r.right()) {
            p->setPen(dim);
            p->drawText(x, baseline, fm.elidedText(command, Qt::ElideRight, r.right() - x));
        }
        p->restore();
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        const auto type = event->type();
        if (isScript(index) && (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease || type == QEvent::MouseButtonDblClick)) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (buttonRect(option).contains(mouse->position().toPoint())) {
                if (type == QEvent::MouseButtonRelease && mouse->button() == Qt::LeftButton && onRun) {
                    onRun(index);
                }
                return true;
            }
        }
        return FileItemDelegate::editorEvent(event, model, option, index);
    }

    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        if (isScript(index) && buttonRect(option).contains(event->pos())) {
            QToolTip::showText(event->globalPos(), NpmScripts::tr("Run"), view);
            return true;
        }
        return FileItemDelegate::helpEvent(event, view, option, index);
    }
};

// "⌄ NPM SCRIPTS" above the list, under a hairline; a click folds the list.
class SectionHeader : public QWidget
{
public:
    explicit SectionHeader(QWidget *parent)
        : QWidget(parent)
    {
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(22, 0, 8, 0); // the chevron is painted in the margin
        auto *title = new QLabel(NpmScripts::tr("NPM SCRIPTS"), this);
        title->setObjectName(QStringLiteral("sideTitle"));
        QFont font = title->font();
        font.setBold(true);
        font.setPointSizeF(font.pointSizeF() * 0.85);
        title->setFont(font);
        title->setMinimumWidth(1);
        layout->addWidget(title);
        layout->addStretch(1);
        setFixedHeight(24);
    }

    bool collapsed = false;
    std::function<void()> onToggle;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        QColor line = palette().color(QPalette::Mid);
        QColor arrow = palette().color(QPalette::WindowText);
        if (Theme::styled()) {
            const Colors &c = Theme::colors();
            line = c.border;
            // the trees' chevrons: 75% of the text colour on the side bar
            arrow = QColor::fromRgbF(c.fg.redF() * 0.75f + c.chrome.redF() * 0.25f,
                                     c.fg.greenF() * 0.75f + c.chrome.greenF() * 0.25f,
                                     c.fg.blueF() * 0.75f + c.chrome.blueF() * 0.25f);
        }
        p.fillRect(QRect(0, 0, width(), 1), line);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(arrow, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.translate(3, (height() - 16) / 2.0 + 0.5);
        QPainterPath path;
        if (collapsed) {
            path.moveTo(6, 4);
            path.lineTo(10, 8);
            path.lineTo(6, 12);
        } else {
            path.moveTo(4, 6);
            path.lineTo(8, 10);
            path.lineTo(12, 6);
        }
        p.drawPath(path);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && onToggle) {
            onToggle();
        }
    }
};
}

// The header and the list of scripts, below the Explorer's file tree.
class NpmScriptsView : public QWidget
{
public:
    NpmScriptsView(NpmScripts *owner, QWidget *parent)
        : QWidget(parent)
        , m_owner(owner)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        header = new SectionHeader(this);
        header->onToggle = [this] {
            m_owner->setCollapsed(!header->collapsed);
        };
        layout->addWidget(header);

        m_model = new QStandardItemModel(this);
        tree = new RoundedTree<QTreeView>(this);
        tree->setModel(m_model);
        tree->setHeaderHidden(true);
        tree->header()->setStretchLastSection(true);
        tree->setUniformRowHeights(true);
        tree->setAnimated(false);
        tree->setIndentation(14);
        tree->setFrameShape(QFrame::NoFrame);
        tree->setIconSize(QSize(16, 16));
        tree->setExpandsOnDoubleClick(false);
        tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        tree->setContextMenuPolicy(Qt::CustomContextMenu);
        tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *delegate = new ScriptDelegate(tree);
        delegate->runIcon = QIcon::fromTheme(QStringLiteral("media-playback-start"));
        delegate->onRun = [this](const QModelIndex &index) {
            activate(index);
        };
        tree->setItemDelegate(delegate);
        layout->addWidget(tree, 1);

        connect(tree, &QTreeView::clicked, this, [this](const QModelIndex &index) {
            if (!isScript(index)) {
                activate(index); // a package row opens or closes
            }
        });
        connect(tree, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
            if (isScript(index)) {
                activate(index);
            }
        });
        connect(tree, &QTreeView::collapsed, this, [this](const QModelIndex &index) {
            m_folded.insert(index.data(PathRole).toString());
        });
        connect(tree, &QTreeView::expanded, this, [this](const QModelIndex &index) {
            m_folded.remove(index.data(PathRole).toString());
        });
        connect(tree, &QTreeView::customContextMenuRequested, this, &NpmScriptsView::showMenu);
        for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
            auto *shortcut = new QShortcut(QKeySequence(key), tree, nullptr, nullptr, Qt::WidgetShortcut);
            connect(shortcut, &QShortcut::activated, this, [this] {
                activate(tree->currentIndex());
            });
        }
    }

    void setPackages(const QList<Npm::Package> &packages)
    {
        // Keep the selected row and the scroll position across a reload.
        const QModelIndex current = tree->currentIndex();
        const QString currentDir = current.data(PathRole).toString();
        const QString currentScript = current.data(ScriptRole).toString();
        const int scroll = tree->verticalScrollBar() ? tree->verticalScrollBar()->value() : 0;

        m_model->clear();
        qsizetype shown = 0;
        for (const Npm::Package &package : packages) {
            shown += package.scripts.isEmpty() ? 0 : 1;
        }
        const bool grouped = shown > 1; // a row for each package.json only when there are several
        tree->setRootIsDecorated(grouped);
        const QIcon packageIcon = VsCodeIcons::instance().fileIcon(QStringLiteral("package.json"));
        const QIcon scriptIcon = QIcon::fromTheme(QStringLiteral("run-build"), QIcon::fromTheme(QStringLiteral("system-run")));
        QModelIndex restore;
        for (const Npm::Package &package : packages) {
            if (package.scripts.isEmpty()) {
                continue;
            }
            QStandardItem *parent = m_model->invisibleRootItem();
            if (grouped) {
                auto *item = new QStandardItem(packageIcon, QStringLiteral("package.json"));
                item->setData(FileItem, KindRole);
                item->setData(package.dir, PathRole);
                item->setData(package.relativeDir, DirRole);
                item->setToolTip(package.file());
                m_model->appendRow(item);
                parent = item;
            }
            for (const Npm::Script &script : package.scripts) {
                auto *item = new QStandardItem(scriptIcon, script.name);
                item->setData(FileItem, KindRole);
                item->setData(package.dir, PathRole);
                item->setData(script.name, ScriptRole);
                item->setData(script.command, DirRole);
                item->setData(script.line, LineRole);
                item->setData(script.column, ColumnRole);
                item->setToolTip(script.command);
                parent->appendRow(item);
                if (package.dir == currentDir && script.name == currentScript) {
                    restore = item->index();
                }
            }
        }
        if (grouped) {
            for (int row = 0; row < m_model->rowCount(); ++row) {
                const QModelIndex index = m_model->index(row, 0);
                tree->setExpanded(index, !m_folded.contains(index.data(PathRole).toString()));
            }
        }
        if (restore.isValid()) {
            tree->setCurrentIndex(restore);
        }
        if (tree->verticalScrollBar()) {
            tree->verticalScrollBar()->setValue(scroll);
        }
    }

    void setCollapsed(bool collapsed)
    {
        header->collapsed = collapsed;
        header->update();
        tree->setVisible(!collapsed);
    }

    int headerHeight() const
    {
        return header->height();
    }

    // The header and every row, with a little room below the last one.
    int preferredHeight() const
    {
        int rows = 0;
        for (QModelIndex i = m_model->index(0, 0); i.isValid(); i = tree->indexBelow(i)) {
            ++rows;
        }
        const int rowHeight = std::max(tree->sizeHintForRow(0), fontMetrics().height() + 4);
        return header->height() + rows * rowHeight + 6;
    }

    // The n-th row shown (for the automated checks).
    QModelIndex visibleRow(int n) const
    {
        QModelIndex i = m_model->index(0, 0);
        for (; i.isValid() && n > 0; --n) {
            i = tree->indexBelow(i);
        }
        return i;
    }

    void activate(const QModelIndex &index)
    {
        if (!index.isValid()) {
            return;
        }
        if (isScript(index)) {
            m_owner->run(index.data(PathRole).toString(), index.data(ScriptRole).toString());
        } else {
            tree->setExpanded(index, !tree->isExpanded(index));
        }
    }

    void open(const QModelIndex &index)
    {
        const QString file = index.data(PathRole).toString() + QStringLiteral("/package.json");
        const int line = index.data(LineRole).isValid() ? index.data(LineRole).toInt() : -1;
        const QString script = index.data(ScriptRole).toString();
        Q_EMIT m_owner->openRequested(file, line, line >= 0 ? index.data(ColumnRole).toInt() : 0, line >= 0 ? int(script.size()) : 0);
    }

    SectionHeader *header = nullptr;
    QTreeView *tree = nullptr;

private:
    void showMenu(const QPoint &pos)
    {
        const QModelIndex index = tree->indexAt(pos);
        if (!index.isValid()) {
            return;
        }
        tree->setCurrentIndex(index);
        const QPersistentModelIndex row(index); // the list may be read again meanwhile
        QMenu menu(this);
        if (isScript(index)) {
            menu.addAction(QIcon::fromTheme(QStringLiteral("media-playback-start")), NpmScripts::tr("Run"), this, [this, row] {
                activate(row);
            });
        }
        menu.addAction(VsCodeIcons::instance().fileIcon(QStringLiteral("package.json")), NpmScripts::tr("Open package.json"), this, [this, row] {
            if (row.isValid()) {
                open(row);
            }
        });
        menu.exec(tree->viewport()->mapToGlobal(pos));
    }

    NpmScripts *m_owner;
    QStandardItemModel *m_model = nullptr;
    QSet<QString> m_folded; // package rows closed by the user
};

NpmScripts::NpmScripts(QWidget *explorer, QWidget *files)
    : QObject(explorer)
    , m_explorer(explorer)
    , m_files(files)
{
    m_rescan.setSingleShot(true);
    connect(&m_rescan, &QTimer::timeout, this, &NpmScripts::scan);
    explorer->installEventFilter(this); // shown again: new package.json files appear
}

void NpmScripts::setRoot(const QString &root)
{
    if (root == m_root) {
        return;
    }
    m_root = root;
    m_scanned = false;
    m_packages.clear();
    m_last = LastRun();
    m_userHeight = 0;
    if (m_watcher && !m_watcher->files().isEmpty()) {
        m_watcher->removePaths(m_watcher->files());
    }
    if (m_view) {
        m_view->setPackages({});
        m_view->hide();
    }
    if (!root.isEmpty()) {
        setCollapsed(state().value(collapsedKey(root), false).toBool()); // remembered for each project
    }
    updateActions();
    if (!root.isEmpty()) {
        m_rescan.start(0); // a few small files, read once the window is up
    }
}

void NpmScripts::setRunner(const Runner &runner)
{
    m_runner = runner;
}

void NpmScripts::bindActions(QAction *run, QAction *rerun)
{
    m_runAction = run;
    m_rerunAction = rerun;
    updateActions();
}

bool NpmScripts::hasScripts() const
{
    return std::any_of(m_packages.cbegin(), m_packages.cend(), [](const Npm::Package &p) {
        return !p.scripts.isEmpty();
    });
}

void NpmScripts::updateActions()
{
    if (m_runAction) {
        m_runAction->setEnabled(hasScripts());
    }
    if (m_rerunAction) {
        m_rerunAction->setEnabled(!m_last.dir.isEmpty());
    }
}

void NpmScripts::scan()
{
    m_rescan.stop();
    if (m_root.isEmpty()) {
        return;
    }
    m_scanned = true;
    const QList<Npm::Package> packages = Npm::findPackages(m_root);

    // Only the package.json files are watched (an editor that saves by
    // replacing the file drops the watch: added again here).
    QStringList files;
    for (const Npm::Package &package : packages) {
        files << package.file();
    }
    if (!files.isEmpty() && !m_watcher) {
        m_watcher = new QFileSystemWatcher(this);
        connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this] {
            m_rescan.start(kRescanDelay);
        });
    }
    if (m_watcher) {
        QStringList gone = m_watcher->files();
        gone.removeIf([&files](const QString &f) {
            return files.contains(f);
        });
        if (!gone.isEmpty()) {
            m_watcher->removePaths(gone);
        }
        QStringList added = files;
        added.removeIf([this](const QString &f) {
            return m_watcher->files().contains(f);
        });
        if (!added.isEmpty()) {
            m_watcher->addPaths(added);
        }
    }

    if (packages == m_packages) {
        return;
    }
    m_packages = packages;
    updateActions();
    const bool any = hasScripts();
    if (any) {
        ensureView();
    }
    if (m_view) {
        m_view->setPackages(m_packages);
        m_view->setVisible(any);
        if (any) {
            fit();
        }
    }
}

void NpmScripts::ensureView()
{
    if (m_view) {
        return;
    }
    // The file tree and the section share the height, like VS Code's views.
    auto *layout = qobject_cast<QBoxLayout *>(m_explorer->layout());
    if (!layout) {
        return;
    }
    m_split = new QSplitter(Qt::Vertical, m_explorer);
    m_split->setChildrenCollapsible(false);
    m_split->setHandleWidth(4);
    delete layout->replaceWidget(m_files, m_split);
    m_split->addWidget(m_files);
    m_view = new NpmScriptsView(this, m_split);
    m_split->addWidget(m_view);
    m_files->lower(); // first among the children: findChild<QTreeView *>() on the Explorer still gives the file tree
    m_split->setStretchFactor(0, 1);
    m_split->setStretchFactor(1, 0);
    m_split->installEventFilter(this);
    connect(m_split, &QSplitter::splitterMoved, this, [this] {
        m_userHeight = m_split->sizes().value(1);
    });
    setCollapsed(m_collapsed);
}

void NpmScripts::setCollapsed(bool collapsed)
{
    if (collapsed != m_collapsed && !m_root.isEmpty()) {
        state().setValue(collapsedKey(m_root), collapsed);
    }
    m_collapsed = collapsed;
    if (!m_view) {
        return;
    }
    m_view->setCollapsed(collapsed);
    QSplitterHandle *handle = m_split->handle(1);
    if (handle) {
        handle->setEnabled(!collapsed);
        handle->setCursor(collapsed ? Qt::ArrowCursor : Qt::SplitVCursor);
    }
    if (collapsed) {
        // Just the header: the file tree takes the rest.
        m_view->setMinimumHeight(m_view->headerHeight());
        m_view->setMaximumHeight(m_view->headerHeight());
    } else {
        m_view->setMinimumHeight(m_view->headerHeight() + m_view->fontMetrics().height() + 8);
        m_view->setMaximumHeight(QWIDGETSIZE_MAX);
    }
    fit();
}

// Tall enough for every script, up to 40% of the Explorer (then it
// scrolls), or the height the user dragged it to; just the header when
// folded.
void NpmScripts::fit()
{
    if (!m_view || m_view->isHidden()) {
        return;
    }
    const int total = m_split->height() - m_split->handleWidth();
    if (!m_split->isVisible() || total <= 0) {
        m_fitPending = true; // laid out when shown
        return;
    }
    m_fitPending = false;
    const int least = m_view->minimumHeight();
    int height = least;
    if (!m_collapsed) {
        height = m_userHeight > 0 ? m_userHeight : std::min(m_view->preferredHeight(), std::max(least, total * 2 / 5));
        height = std::clamp(height, least, std::max(least, total - 100));
    }
    m_split->setSizes({total - height, height});
}

bool NpmScripts::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_explorer && event->type() == QEvent::Show && m_scanned) {
        m_rescan.start(0);
    } else if (watched == m_split && (event->type() == QEvent::Show || event->type() == QEvent::Resize) && m_fitPending) {
        QTimer::singleShot(0, this, &NpmScripts::fit);
    }
    return QObject::eventFilter(watched, event);
}

QList<CommandChoice> NpmScripts::choices()
{
    if (!m_scanned) {
        scan();
    }
    delete m_choiceActions;
    m_choiceActions = new QObject(this);
    QList<CommandChoice> choices;
    for (const Npm::Package &package : std::as_const(m_packages)) {
        for (const Npm::Script &script : package.scripts) {
            auto *action = new QAction(m_choiceActions);
            connect(action, &QAction::triggered, this, [this, dir = package.dir, name = script.name] {
                run(dir, name);
            });
            CommandChoice choice;
            choice.action = action;
            // "start", or "start - web" for a package.json in a folder, as in VS Code
            choice.title = package.relativeDir.isEmpty() ? script.name : script.name + QStringLiteral(" - ") + package.relativeDir;
            choice.detail = script.command;
            choices << choice;
        }
    }
    return choices;
}

void NpmScripts::run(const QString &dir, const QString &script, bool again)
{
    if (!m_runner || dir.isEmpty() || script.isEmpty()) {
        return;
    }
    QString runner = QStringLiteral("npm");
    for (const Npm::Package &package : std::as_const(m_packages)) {
        if (package.dir == dir) {
            runner = package.runner;
        }
    }
    QWidget *reuse = again && m_last.dir == dir && m_last.script == script ? m_last.terminal.data() : nullptr;
    QWidget *terminal = m_runner(dir, script, Npm::commandLine(runner, script), reuse);
    m_last = {dir, script, terminal};
    updateActions();
}

void NpmScripts::rerunLast()
{
    if (!m_last.dir.isEmpty()) {
        run(m_last.dir, m_last.script, true);
    }
}

// LIEVE_TEST_NPM steps, one every 600 ms: "print" (packages and commands
// on stdout), "hover:N" (the N-th row shown), "select:N", "run:N" (as
// Enter), "click:N" (its run button), "toggle" (the header), "pick[:text]"
// (Tasks: Run npm Script… with that text typed), "enter" (in the picker),
// "rerun", "open:N" (Open package.json).
void NpmScripts::runTest(const QString &steps)
{
    const QStringList list = steps.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (qsizetype i = 0; i < list.size(); ++i) {
        QTimer::singleShot(int(i + 1) * 600, this, [this, step = list.at(i)] {
            testStep(step);
        });
    }
}

void NpmScripts::testStep(const QString &step)
{
    const QString name = step.section(QLatin1Char(':'), 0, 0);
    const QString arg = step.section(QLatin1Char(':'), 1);
    qInfo().noquote() << "npm-test" << step;
    if (name == QLatin1String("print")) {
        for (const Npm::Package &package : std::as_const(m_packages)) {
            qInfo().noquote() << "npm-package" << (package.relativeDir.isEmpty() ? QStringLiteral(".") : package.relativeDir) << package.runner
                              << package.name << package.scripts.size();
            for (const Npm::Script &script : package.scripts) {
                qInfo().noquote() << "npm-script" << script.name << "|" << script.command << "|" << Npm::commandLine(package.runner, script.name)
                                  << "|" << script.line + 1 << script.column + 1;
            }
        }
        qInfo().noquote() << "npm-section" << (m_view && m_view->isVisible() ? "shown" : "hidden") << (m_collapsed ? "collapsed" : "expanded")
                          << (m_split ? m_split->sizes() : QList<int>());
        return;
    }
    if (name == QLatin1String("pick")) {
        if (m_runAction) {
            m_runAction->trigger();
        }
        QTimer::singleShot(200, this, [this, arg] {
            auto *quickOpen = m_explorer->window()->findChild<QuickOpen *>();
            if (auto *edit = quickOpen ? quickOpen->findChild<QLineEdit *>() : nullptr; edit && !arg.isEmpty()) {
                edit->setText(arg);
            }
        });
        return;
    }
    if (name == QLatin1String("enter")) {
        auto *quickOpen = m_explorer->window()->findChild<QuickOpen *>();
        if (auto *edit = quickOpen ? quickOpen->findChild<QLineEdit *>() : nullptr) {
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QCoreApplication::sendEvent(edit, &enter);
        }
        return;
    }
    if (name == QLatin1String("rerun")) {
        if (m_rerunAction) {
            m_rerunAction->trigger();
        }
        return;
    }
    if (!m_view) {
        qInfo().noquote() << "npm-test: no section";
        return;
    }
    if (name == QLatin1String("toggle")) {
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(40, 10), m_view->header->mapToGlobal(QPointF(40, 10)), Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(m_view->header, &press);
        return;
    }
    QTreeView *tree = m_view->tree;
    const QModelIndex index = m_view->visibleRow(arg.toInt());
    const QRect rect = tree->visualRect(index);
    if (name == QLatin1String("hover")) {
        const QPointF pos = QRectF(rect).center();
        QHoverEvent event(QEvent::HoverMove, pos, tree->viewport()->mapToGlobal(pos), QPointF(-1, -1));
        QCoreApplication::sendEvent(tree->viewport(), &event);
    } else if (name == QLatin1String("select")) {
        tree->setFocus();
        tree->setCurrentIndex(index);
    } else if (name == QLatin1String("run")) {
        m_view->activate(index);
    } else if (name == QLatin1String("open")) {
        m_view->open(index);
    } else if (name == QLatin1String("click")) {
        QStyleOptionViewItem option;
        option.rect = QRect(0, rect.top(), tree->viewport()->width(), rect.height());
        const QPointF pos = QRectF(ScriptDelegate::buttonRect(option)).center();
        for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent event(type, pos, tree->viewport()->mapToGlobal(pos), Qt::LeftButton, type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(tree->viewport(), &event);
        }
    }
}
