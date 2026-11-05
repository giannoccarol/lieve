#include "gitpanel.h"

#include "settings.h"
#include "theme.h"
#include "vscodeicons.h"

#include <QApplication>
#include <QClipboard>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>

using namespace Lieve;

namespace
{
enum Role {
    KindRole = Qt::UserRole + 1,
    GroupRole,
    StatusRole,
    PathRole,
    OrigRole,
    DirRole,
    CountRole,
};
enum Kind {
    SectionRow,
    FileRow,
};

QColor statusColor(QChar status)
{
    const bool dark = isDarkTheme();
    switch (status.unicode()) {
    case 'A':
    case 'U':
    case 'R':
    case 'C':
        return dark ? QColor(0x73, 0xc9, 0x91) : QColor(0x58, 0x7c, 0x0c);
    case 'D':
        return dark ? QColor(0xc7, 0x4e, 0x39) : QColor(0xad, 0x07, 0x07);
    case '!':
        return dark ? QColor(0xe4, 0x67, 0x6b) : QColor(0xad, 0x07, 0x07);
    default: // M, T
        return dark ? QColor(0xe2, 0xc0, 0x8d) : QColor(0x89, 0x55, 0x03);
    }
}

QString statusText(QChar status)
{
    switch (status.unicode()) {
    case 'A':
        return QStringLiteral("Aggiunto");
    case 'U':
        return QStringLiteral("Non tracciato");
    case 'D':
        return QStringLiteral("Eliminato");
    case 'R':
        return QStringLiteral("Rinominato");
    case 'C':
        return QStringLiteral("Copiato");
    case 'T':
        return QStringLiteral("Tipo cambiato");
    case '!':
        return QStringLiteral("Conflitto");
    default:
        return QStringLiteral("Modificato");
    }
}

QList<GitPanel::Action> actionsFor(const QModelIndex &index)
{
    const auto group = GitEntry::Group(index.data(GroupRole).toInt());
    if (index.data(KindRole).toInt() == SectionRow) {
        switch (group) {
        case GitEntry::Staged:
            return {GitPanel::Unstage};
        case GitEntry::Changes:
            return {GitPanel::Discard, GitPanel::Stage};
        case GitEntry::Conflicts:
            return {GitPanel::Stage};
        }
        return {};
    }
    const bool deleted = index.data(StatusRole).toString() == QLatin1String("D");
    QList<GitPanel::Action> actions;
    if (!deleted) {
        actions << GitPanel::OpenFile;
    }
    switch (group) {
    case GitEntry::Staged:
        actions << GitPanel::Unstage;
        break;
    case GitEntry::Changes:
        actions << GitPanel::Discard << GitPanel::Stage;
        break;
    case GitEntry::Conflicts:
        actions << GitPanel::Stage;
        break;
    }
    return actions;
}

QIcon actionIcon(GitPanel::Action action)
{
    switch (action) {
    case GitPanel::OpenFile:
        return QIcon::fromTheme(QStringLiteral("document-open"));
    case GitPanel::Discard:
        return QIcon::fromTheme(QStringLiteral("edit-undo"));
    case GitPanel::Stage:
        return QIcon::fromTheme(QStringLiteral("list-add"));
    case GitPanel::Unstage:
        return QIcon::fromTheme(QStringLiteral("list-remove"));
    }
    return {};
}

QString actionTip(GitPanel::Action action, bool section)
{
    switch (action) {
    case GitPanel::OpenFile:
        return QStringLiteral("Apri il file");
    case GitPanel::Discard:
        return section ? QStringLiteral("Annulla tutte le modifiche") : QStringLiteral("Annulla le modifiche");
    case GitPanel::Stage:
        return section ? QStringLiteral("Prepara tutte per il commit") : QStringLiteral("Prepara per il commit");
    case GitPanel::Unstage:
        return section ? QStringLiteral("Rimuovi tutte dalla preparazione") : QStringLiteral("Rimuovi dalla preparazione");
    }
    return {};
}

// Rows: [icon] name  dim/dir ......... [hover actions] M
//       Section title ................ [hover actions] (12)
class GitDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    std::function<void(const QModelIndex &, GitPanel::Action)> onAction;
    bool actionClicked = false; // the last click hit an action button, not the row

    static int rightReserve(const QModelIndex &index, const QFontMetrics &fm)
    {
        if (index.data(KindRole).toInt() == SectionRow) {
            return fm.horizontalAdvance(QString::number(index.data(CountRole).toInt())) + 14;
        }
        return 16;
    }

    static QList<QPair<GitPanel::Action, QRect>> actionRects(const QStyleOptionViewItem &option, const QModelIndex &index)
    {
        QList<QPair<GitPanel::Action, QRect>> rects;
        const QList<GitPanel::Action> actions = actionsFor(index);
        int x = option.rect.right() - 4 - rightReserve(index, option.fontMetrics) - 6;
        for (auto it = actions.crbegin(); it != actions.crend(); ++it) {
            const QRect r(x - 20, option.rect.center().y() - 10, 20, 20);
            rects.prepend({*it, r});
            x -= 22;
        }
        return rects;
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(std::max(size.height(), option.fontMetrics.height() + 8));
        return size;
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const bool section = index.data(KindRole).toInt() == SectionRow;
        const QString text = opt.text;
        opt.text.clear();
        if (section) {
            opt.icon = QIcon();
        }
        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, p, widget);

        const bool selected = opt.state & QStyle::State_Selected;
        const bool hovered = opt.state & QStyle::State_MouseOver;
        const QColor fg = opt.palette.color(selected && !Theme::styled() ? QPalette::HighlightedText : QPalette::Text);
        QColor dim = fg;
        dim.setAlphaF(0.55f);

        QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget).adjusted(2, 0, 0, 0);
        const int reserve = rightReserve(index, opt.fontMetrics);
        int textRight = opt.rect.right() - 4 - reserve - 6;

        p->save();
        if (hovered) {
            const auto rects = actionRects(opt, index);
            for (const auto &[action, rect] : rects) {
                actionIcon(action).paint(p, rect.adjusted(2, 2, -2, -2));
            }
            if (!rects.isEmpty()) {
                textRight = rects.first().second.left() - 4;
            }
        }
        textRect.setRight(textRight);

        QFont font = opt.font;
        const QFontMetrics fm(font);
        const int baseline = textRect.top() + (textRect.height() - fm.height()) / 2 + fm.ascent();

        if (section) {
            QFont bold = font;
            bold.setBold(true);
            p->setFont(bold);
            p->setPen(fg);
            p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(bold).elidedText(text, Qt::ElideRight, textRect.width()));
            // Count badge
            const QString count = QString::number(index.data(CountRole).toInt());
            const int w = fm.horizontalAdvance(count) + 12;
            const QRect badge(opt.rect.right() - 4 - w, opt.rect.center().y() - fm.height() / 2, w, fm.height());
            // Neutral counter, like VS Code's
            QColor bg = fg;
            bg.setAlphaF(0.16f);
            p->setRenderHint(QPainter::Antialiasing);
            p->setPen(Qt::NoPen);
            p->setBrush(bg);
            p->drawRoundedRect(badge, badge.height() / 2.0, badge.height() / 2.0);
            p->setFont(font);
            p->setPen(fg);
            p->drawText(badge, Qt::AlignCenter, count);
        } else {
            const QString statusString = index.data(StatusRole).toString();
            const QChar status = statusString.isEmpty() ? QChar() : statusString.at(0);
            QFont nameFont = font;
            nameFont.setStrikeOut(status == QLatin1Char('D'));
            p->setFont(nameFont);
            p->setPen(fg);
            const QString name = QFontMetrics(nameFont).elidedText(text, Qt::ElideRight, textRect.width());
            p->drawText(textRect.left(), baseline, name);
            const int x = textRect.left() + QFontMetrics(nameFont).horizontalAdvance(name) + 8;
            const QString dir = index.data(DirRole).toString();
            if (!dir.isEmpty() && x < textRect.right()) {
                p->setFont(font);
                p->setPen(dim);
                p->drawText(x, baseline, fm.elidedText(dir, Qt::ElideMiddle, textRect.right() - x));
            }
            QFont bold = font;
            bold.setBold(true);
            p->setFont(bold);
            p->setPen(selected ? fg : statusColor(status));
            const QRect letter(opt.rect.right() - 4 - reserve, opt.rect.top(), reserve, opt.rect.height());
            p->drawText(letter, Qt::AlignCenter, QString(status));
        }
        p->restore();
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        const auto type = event->type();
        if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease || type == QEvent::MouseButtonDblClick) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            const auto rects = actionRects(option, index);
            for (const auto &[action, rect] : rects) {
                if (rect.contains(mouse->position().toPoint())) {
                    if (type == QEvent::MouseButtonRelease && mouse->button() == Qt::LeftButton && onAction) {
                        actionClicked = true;
                        onAction(index, action);
                    }
                    return true;
                }
            }
        }
        if (type == QEvent::ToolTip) {
            return false;
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }

    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        const auto rects = actionRects(option, index);
        for (const auto &[action, rect] : rects) {
            if (rect.contains(event->pos())) {
                QToolTip::showText(event->globalPos(), actionTip(action, index.data(KindRole).toInt() == SectionRow), view);
                return true;
            }
        }
        return QStyledItemDelegate::helpEvent(event, view, option, index);
    }
};

QToolButton *headerButton(const QString &icon, const QString &tip, QWidget *parent)
{
    auto *b = new QToolButton(parent);
    b->setIcon(QIcon::fromTheme(icon));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setIconSize(QSize(16, 16));
    return b;
}
}

GitPanel::GitPanel(GitRepo *repo, QWidget *parent)
    : QWidget(parent)
    , m_repo(repo)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 0);
    layout->setSpacing(6);

    // Header
    auto *header = new QHBoxLayout;
    header->setContentsMargins(4, 0, 0, 0);
    auto *title = new QLabel(QStringLiteral("CONTROLLO SORGENTE"), this);
    title->setObjectName(QStringLiteral("sideTitle"));
    QFont titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() * 0.85);
    title->setFont(titleFont);
    title->setMinimumWidth(1);
    header->addWidget(title, 1);
    auto *refresh = headerButton(QStringLiteral("view-refresh"), QStringLiteral("Aggiorna"), this);
    auto *more = headerButton(QStringLiteral("overflow-menu"), QStringLiteral("Altre azioni"), this);
    if (more->icon().isNull()) {
        more->setIcon(QIcon::fromTheme(QStringLiteral("application-menu")));
    }
    more->setPopupMode(QToolButton::InstantPopup);
    header->addWidget(refresh);
    header->addWidget(more);
    layout->addLayout(header);

    m_branch = new QLabel(this);
    m_branch->setForegroundRole(QPalette::PlaceholderText);
    m_branch->setContentsMargins(4, 0, 0, 0);
    layout->addWidget(m_branch);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(3);
    m_progress->hide();
    layout->addWidget(m_progress);

    m_stack = new QStackedWidget(this);
    layout->addWidget(m_stack, 1);

    // Page 0: repository
    auto *repoPage = new QWidget(m_stack);
    auto *rl = new QVBoxLayout(repoPage);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(6);
    m_message = new QPlainTextEdit(repoPage);
    m_message->setObjectName(QStringLiteral("commitMessage"));
    m_message->setTabChangesFocus(true);
    m_message->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_message->installEventFilter(this);
    rl->addWidget(m_message);

    auto *commitRow = new QHBoxLayout;
    commitRow->setSpacing(2);
    m_commit = new QPushButton(repoPage);
    m_commit->setObjectName(QStringLiteral("primaryButton"));
    m_commit->setDefault(true);
    m_commit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_commitMenu = new QToolButton(repoPage);
    m_commitMenu->setObjectName(QStringLiteral("primarySplit"));
    m_commitMenu->setPopupMode(QToolButton::InstantPopup);
    m_commitMenu->setToolTip(QStringLiteral("Altre azioni di commit"));
    m_commitMenu->setFixedHeight(m_commit->sizeHint().height());
    commitRow->addWidget(m_commit, 1);
    commitRow->addWidget(m_commitMenu);
    rl->addLayout(commitRow);

    m_tree = new QTreeWidget(repoPage);
    m_tree->setHeaderHidden(true);
    m_tree->setColumnCount(1);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setIndentation(12);
    m_tree->setIconSize(QSize(16, 16));
    m_tree->setUniformRowHeights(true);
    m_tree->setMouseTracking(true);
    m_tree->setExpandsOnDoubleClick(false);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    auto *delegate = new GitDelegate(m_tree);
    m_tree->setItemDelegate(delegate);
    rl->addWidget(m_tree, 1);
    m_stack->addWidget(repoPage);

    // Page 1: not a repository
    auto *emptyPage = new QWidget(m_stack);
    auto *el = new QVBoxLayout(emptyPage);
    el->setContentsMargins(4, 8, 4, 8);
    m_noRepo = new QLabel(emptyPage);
    m_noRepo->setWordWrap(true);
    m_init = new QPushButton(QIcon::fromTheme(QStringLiteral("vcs-branch")), QStringLiteral("Inizializza repository"), emptyPage);
    el->addWidget(m_noRepo);
    el->addWidget(m_init);
    el->addStretch(1);
    m_stack->addWidget(emptyPage);

    // Menus
    auto *commitMenu = new QMenu(this);
    commitMenu->addAction(QIcon::fromTheme(QStringLiteral("dialog-ok-apply")), QStringLiteral("Commit"), this, [this] {
        commit(GitRepo::After::Nothing, false);
    });
    commitMenu->addAction(QStringLiteral("Commit (modifica l'ultimo)"), this, [this] {
        commit(GitRepo::After::Nothing, true);
    });
    commitMenu->addSeparator();
    commitMenu->addAction(QIcon::fromTheme(QStringLiteral("go-up")), QStringLiteral("Commit e push"), this, [this] {
        commit(GitRepo::After::Push, false);
    });
    commitMenu->addAction(QIcon::fromTheme(QStringLiteral("view-refresh")), QStringLiteral("Commit e sincronizza"), this, [this] {
        commit(GitRepo::After::Sync, false);
    });
    m_commitMenu->setMenu(commitMenu);

    auto *moreMenu = new QMenu(this);
    moreMenu->addAction(QIcon::fromTheme(QStringLiteral("view-refresh")), QStringLiteral("Sincronizza (pull e push)"), m_repo, &GitRepo::sync);
    moreMenu->addAction(QIcon::fromTheme(QStringLiteral("go-down")), QStringLiteral("Pull"), m_repo, &GitRepo::pull);
    moreMenu->addAction(QIcon::fromTheme(QStringLiteral("go-up")), QStringLiteral("Push"), m_repo, &GitRepo::push);
    moreMenu->addAction(QIcon::fromTheme(QStringLiteral("download")), QStringLiteral("Recupera (fetch)"), this, [this] {
        m_repo->fetch(false);
    });
    moreMenu->addSeparator();
    moreMenu->addAction(QIcon::fromTheme(QStringLiteral("list-add")), QStringLiteral("Prepara tutte le modifiche"), this, [this] {
        QStringList paths;
        for (const GitEntry &e : m_repo->entries()) {
            if (e.group != GitEntry::Staged) {
                paths << e.path;
            }
        }
        m_repo->stage(paths);
    });
    moreMenu->addAction(QIcon::fromTheme(QStringLiteral("list-remove")), QStringLiteral("Rimuovi tutte dalla preparazione"), this, [this] {
        m_repo->unstage(m_repo->entries());
    });
    moreMenu->addSeparator();
    moreMenu->addAction(QIcon::fromTheme(QStringLiteral("edit-undo")), QStringLiteral("Annulla l'ultimo commit"), this, [this] {
        if (!m_repo->upstream().isEmpty() && m_repo->ahead() == 0) {
            const auto answer = QMessageBox::question(this,
                                                      QStringLiteral("Annulla l'ultimo commit"),
                                                      QStringLiteral("L'ultimo commit è già stato inviato al server. Annullarlo comunque?\n"
                                                                     "Le modifiche restano preparate e il messaggio torna nella casella."));
            if (answer != QMessageBox::Yes) {
                return;
            }
        }
        m_repo->undoLastCommit();
    });
    more->setMenu(moreMenu);

    // Wiring
    delegate->onAction = [this](const QModelIndex &index, Action action) {
        runAction(m_tree->itemFromIndex(index), action);
    };
    connect(m_tree, &QTreeWidget::itemClicked, this, [this, delegate](QTreeWidgetItem *item) {
        if (delegate->actionClicked) {
            delegate->actionClicked = false;
            return;
        }
        activate(item);
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        if (item->data(0, KindRole).toInt() == FileRow && item->data(0, StatusRole).toString() != QLatin1String("D")) {
            Q_EMIT openFileRequested(m_repo->absolutePath(item->data(0, PathRole).toString()));
        }
    });
    connect(m_tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
        m_collapsed.insert(item->data(0, GroupRole).toInt());
    });
    connect(m_tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        m_collapsed.remove(item->data(0, GroupRole).toInt());
    });
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &GitPanel::showMenu);
    for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
        auto *sc = new QShortcut(QKeySequence(key), m_tree, nullptr, nullptr, Qt::WidgetShortcut);
        connect(sc, &QShortcut::activated, this, [this] {
            activate(m_tree->currentItem());
        });
    }
    connect(m_message, &QPlainTextEdit::textChanged, this, &GitPanel::adjustMessageHeight);
    connect(m_commit, &QPushButton::clicked, this, &GitPanel::primaryAction);
    connect(refresh, &QToolButton::clicked, m_repo, &GitRepo::refresh);
    connect(m_init, &QPushButton::clicked, m_repo, &GitRepo::init);

    connect(m_repo, &GitRepo::changed, this, &GitPanel::rebuild);
    connect(m_repo, &GitRepo::busyChanged, this, [this](bool busy) {
        m_progress->setVisible(busy);
        updateCommitButton();
    });
    connect(m_repo, &GitRepo::committed, m_message, &QPlainTextEdit::clear);
    connect(m_repo, &GitRepo::messageRestored, m_message, &QPlainTextEdit::setPlainText);
    connect(m_repo, &GitRepo::failed, this, [this](const QString &title, const QString &details) {
        QMessageBox box(QMessageBox::Warning, title, title, QMessageBox::Ok, this);
        const QStringList lines = details.split(QLatin1Char('\n'));
        box.setInformativeText(lines.mid(0, 8).join(QLatin1Char('\n')));
        if (lines.size() > 8) {
            box.setDetailedText(details);
        }
        box.exec();
    });

    adjustMessageHeight();
    rebuild();
}

void GitPanel::focusMessage()
{
    if (m_stack->currentIndex() == 0) {
        m_message->setFocus();
    }
}

void GitPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_repo->refresh();
}

bool GitPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_message && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && (key->modifiers() & Qt::ControlModifier)) {
            primaryAction();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void GitPanel::adjustMessageHeight()
{
    // QPlainTextEdit's document height is in lines.
    const int lines = std::clamp(int(m_message->document()->size().height()), 2, 8);
    const int margins = int(m_message->document()->documentMargin() * 2) + m_message->frameWidth() * 2;
    m_message->setFixedHeight(lines * m_message->fontMetrics().lineSpacing() + margins + 2);
}

void GitPanel::rebuild()
{
    if (!GitRepo::gitInstalled()) {
        m_noRepo->setText(QStringLiteral("git non è installato."));
        m_init->hide();
        m_branch->clear();
        m_stack->setCurrentIndex(1);
        return;
    }
    if (!m_repo->isRepository()) {
        m_noRepo->setText(m_repo->directory().isEmpty() ? QStringLiteral("Nessun progetto aperto.")
                                                        : QStringLiteral("Questa cartella non è un repository git."));
        m_init->setVisible(!m_repo->directory().isEmpty());
        m_branch->clear();
        m_stack->setCurrentIndex(1);
        return;
    }
    m_stack->setCurrentIndex(0);

    // Branch line
    QString branch = m_repo->branch();
    if (branch == QLatin1String("(detached)")) {
        branch = QStringLiteral("HEAD staccato");
    }
    QString line = branch;
    if (!m_repo->upstream().isEmpty()) {
        line += QStringLiteral("   ↓%1 ↑%2").arg(m_repo->behind()).arg(m_repo->ahead());
    } else if (!m_repo->unborn()) {
        line += QStringLiteral("   (non pubblicato)");
    }
    m_branch->setText(line);
    m_message->setPlaceholderText(QStringLiteral("Messaggio (Ctrl+Invio per il commit su “%1”)").arg(branch));

    // Keep the selection and the scroll position across refreshes
    QString selected;
    if (QTreeWidgetItem *current = m_tree->currentItem()) {
        selected = current->data(0, GroupRole).toString() + QLatin1Char(':') + current->data(0, PathRole).toString();
    }
    const int scroll = m_tree->verticalScrollBar()->value();
    m_tree->setUpdatesEnabled(false);
    m_tree->clear();

    const QList<GitEntry> &entries = m_repo->entries();
    const QPair<GitEntry::Group, QString> sections[] = {
        {GitEntry::Conflicts, QStringLiteral("Conflitti di unione")},
        {GitEntry::Staged, QStringLiteral("Modifiche preparate per il commit")},
        {GitEntry::Changes, QStringLiteral("Modifiche")},
    };
    QTreeWidgetItem *toSelect = nullptr;
    for (const auto &[group, title] : sections) {
        int count = 0;
        for (const GitEntry &e : entries) {
            count += e.group == group ? 1 : 0;
        }
        if (count == 0) {
            continue;
        }
        auto *top = new QTreeWidgetItem(m_tree);
        top->setText(0, title);
        top->setData(0, KindRole, SectionRow);
        top->setData(0, GroupRole, int(group));
        top->setData(0, CountRole, count);
        top->setFlags(Qt::ItemIsEnabled);
        for (const GitEntry &e : entries) {
            if (e.group != group) {
                continue;
            }
            const QFileInfo fi(e.path);
            auto *item = new QTreeWidgetItem(top);
            item->setText(0, fi.fileName());
            item->setIcon(0, VsCodeIcons::instance().fileIcon(fi.fileName()));
            item->setData(0, KindRole, FileRow);
            item->setData(0, GroupRole, int(group));
            item->setData(0, StatusRole, QString(e.status));
            item->setData(0, PathRole, e.path);
            item->setData(0, OrigRole, e.origPath);
            QString dir = fi.path() == QLatin1String(".") ? QString() : fi.path();
            if (!e.origPath.isEmpty()) {
                dir = QStringLiteral("da %1").arg(e.origPath);
            }
            item->setData(0, DirRole, dir);
            item->setToolTip(0, QStringLiteral("%1 • %2").arg(e.path, statusText(e.status)));
            if (QString::number(int(group)) + QLatin1Char(':') + e.path == selected) {
                toSelect = item;
            }
        }
        top->setExpanded(!m_collapsed.contains(int(group)));
    }
    if (toSelect) {
        m_tree->setCurrentItem(toSelect);
    }
    m_tree->setUpdatesEnabled(true);
    m_tree->verticalScrollBar()->setValue(scroll);
    updateCommitButton();
}

void GitPanel::updateCommitButton()
{
    const bool busy = m_repo->isBusy();
    const bool changes = !m_repo->entries().isEmpty();
    const QString branch = m_repo->branch();
    QString mode = QStringLiteral("commit");
    if (!changes) {
        if (m_repo->upstream().isEmpty() && !m_repo->unborn() && !branch.isEmpty() && branch != QLatin1String("(detached)")) {
            mode = QStringLiteral("publish");
        } else if (m_repo->ahead() > 0 || m_repo->behind() > 0) {
            mode = QStringLiteral("sync");
        }
    }
    m_commit->setProperty("mode", mode);
    if (mode == QLatin1String("publish")) {
        m_commit->setIcon(QIcon::fromTheme(QStringLiteral("cloud-upload"), QIcon::fromTheme(QStringLiteral("go-up"))));
        m_commit->setText(QStringLiteral("Pubblica branch"));
    } else if (mode == QLatin1String("sync")) {
        m_commit->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
        m_commit->setText(QStringLiteral("Sincronizza modifiche  ↓%1 ↑%2").arg(m_repo->behind()).arg(m_repo->ahead()));
    } else {
        m_commit->setIcon(QIcon::fromTheme(QStringLiteral("dialog-ok-apply")));
        m_commit->setText(QStringLiteral("Commit"));
    }
    m_commit->setEnabled(!busy && (changes || mode != QLatin1String("commit")));
    m_commitMenu->setEnabled(!busy);
}

void GitPanel::primaryAction()
{
    const QString mode = m_commit->property("mode").toString();
    if (m_repo->isBusy()) {
        return;
    }
    if (mode == QLatin1String("publish")) {
        m_repo->push();
    } else if (mode == QLatin1String("sync")) {
        m_repo->sync();
    } else {
        commit(GitRepo::After::Nothing, false);
    }
}

void GitPanel::commit(GitRepo::After after, bool amend)
{
    const QString message = m_message->toPlainText().trimmed();
    if (message.isEmpty() && !amend) {
        m_message->setFocus();
        QToolTip::showText(m_message->mapToGlobal(QPoint(0, m_message->height())), QStringLiteral("Scrivi un messaggio per il commit."), m_message);
        return;
    }
    bool stageAll = false;
    if (!amend && !m_repo->hasGroup(GitEntry::Staged)) {
        if (!m_repo->hasGroup(GitEntry::Changes)) {
            return;
        }
        // Same as VS Code's git.enableSmartCommit
        if (configOrVscode(QStringLiteral("git/smartCommit"), m_repo->directory(), QStringLiteral("git.enableSmartCommit"), false)) {
            stageAll = true;
        } else {
            const auto answer = QMessageBox::question(this,
                                                      QStringLiteral("Commit"),
                                                      QStringLiteral("Non ci sono modifiche preparate per il commit.\n"
                                                                     "Vuoi preparare tutte le modifiche e fare il commit?"));
            if (answer != QMessageBox::Yes) {
                return;
            }
            stageAll = true;
        }
    }
    m_repo->commit(message, amend, stageAll, after);
}

QList<GitEntry> GitPanel::entriesOf(QTreeWidgetItem *item) const
{
    QList<GitEntry> result;
    if (!item) {
        return result;
    }
    const auto group = GitEntry::Group(item->data(0, GroupRole).toInt());
    if (item->data(0, KindRole).toInt() == SectionRow) {
        for (const GitEntry &e : m_repo->entries()) {
            if (e.group == group) {
                result << e;
            }
        }
        return result;
    }
    GitEntry e;
    e.group = group;
    const QString status = item->data(0, StatusRole).toString();
    e.status = status.isEmpty() ? QChar() : status.at(0);
    e.path = item->data(0, PathRole).toString();
    e.origPath = item->data(0, OrigRole).toString();
    result << e;
    return result;
}

bool GitPanel::confirmDiscard(const QList<GitEntry> &entries)
{
    QList<GitEntry> changes;
    for (const GitEntry &e : entries) {
        if (e.group == GitEntry::Changes) {
            changes << e;
        }
    }
    if (changes.isEmpty()) {
        return false;
    }
    const int untracked = int(std::count_if(changes.cbegin(), changes.cend(), [](const GitEntry &e) {
        return e.status == QLatin1Char('U');
    }));
    QString text;
    if (changes.size() == 1) {
        const QString name = QFileInfo(changes.first().path).fileName();
        text = untracked ? QStringLiteral("Spostare nel cestino il file nuovo «%1»?").arg(name)
                         : QStringLiteral("Annullare le modifiche a «%1»?\nLe modifiche andranno perse.").arg(name);
    } else {
        text = QStringLiteral("Annullare le modifiche a %1 file?").arg(changes.size());
        if (untracked > 0) {
            text += QStringLiteral("\n%1 file nuovi verranno spostati nel cestino.").arg(untracked);
        }
        if (untracked < changes.size()) {
            text += QStringLiteral("\nLe modifiche agli altri file andranno perse.");
        }
    }
    QMessageBox box(QMessageBox::Warning, QStringLiteral("Annulla modifiche"), text, QMessageBox::Cancel, this);
    QPushButton *confirm = box.addButton(QStringLiteral("Annulla modifiche"), QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    return box.clickedButton() == confirm;
}

void GitPanel::runAction(QTreeWidgetItem *item, Action action)
{
    const QList<GitEntry> entries = entriesOf(item);
    if (entries.isEmpty()) {
        return;
    }
    switch (action) {
    case OpenFile:
        Q_EMIT openFileRequested(m_repo->absolutePath(entries.first().path));
        break;
    case Stage: {
        QStringList paths;
        for (const GitEntry &e : entries) {
            paths << e.path;
        }
        m_repo->stage(paths);
        break;
    }
    case Unstage:
        m_repo->unstage(entries);
        break;
    case Discard:
        if (confirmDiscard(entries)) {
            m_repo->discard(entries);
        }
        break;
    }
}

void GitPanel::activate(QTreeWidgetItem *item)
{
    if (!item) {
        return;
    }
    if (item->data(0, KindRole).toInt() == SectionRow) {
        item->setExpanded(!item->isExpanded());
        return;
    }
    Q_EMIT diffRequested(entriesOf(item).first());
}

void GitPanel::showMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_tree->itemAt(pos);
    if (!item) {
        return;
    }
    const bool section = item->data(0, KindRole).toInt() == SectionRow;
    const auto group = GitEntry::Group(item->data(0, GroupRole).toInt());
    QMenu menu(this);
    if (!section) {
        menu.addAction(QIcon::fromTheme(QStringLiteral("view-split-left-right"), QIcon::fromTheme(QStringLiteral("document-preview"))),
                       QStringLiteral("Mostra le differenze"),
                       this,
                       [this, item] {
                           activate(item);
                       });
        if (item->data(0, StatusRole).toString() != QLatin1String("D")) {
            menu.addAction(QIcon::fromTheme(QStringLiteral("document-open")), QStringLiteral("Apri il file"), this, [this, item] {
                runAction(item, OpenFile);
            });
        }
        menu.addSeparator();
    }
    const auto actions = actionsFor(m_tree->indexFromItem(item));
    for (const Action action : actions) {
        if (action == OpenFile) {
            continue;
        }
        menu.addAction(actionIcon(action), actionTip(action, section), this, [this, item, action] {
            runAction(item, action);
        });
    }
    if (!section) {
        menu.addSeparator();
        const QString path = item->data(0, PathRole).toString();
        menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), QStringLiteral("Copia percorso"), this, [this, path] {
            QApplication::clipboard()->setText(m_repo->absolutePath(path));
        });
        menu.addAction(QStringLiteral("Copia percorso relativo"), this, [path] {
            QApplication::clipboard()->setText(path);
        });
    }
    Q_UNUSED(group);
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}
