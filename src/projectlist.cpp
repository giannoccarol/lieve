#include "projectlist.h"

#include "fuzzy.h"
#include "jumplist.h"
#include "theme.h"
#include "windowmanager.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>

using namespace Lieve;

namespace
{
enum Role {
    PathRole = Qt::UserRole + 1,
    DirRole,
    BranchRole,
    TimeRole,
    OpenRole,
    PinnedRole,
};

// Two-line row:  [icon]  Name  📌                   2 hours ago / ● open
//                        ~/path/to/project                       develop
class ProjectDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        return QSize(200, option.fontMetrics.height() * 2 + 16);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QIcon icon = opt.icon;
        const QString name = opt.text;
        opt.text.clear();
        opt.icon = QIcon();
        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, p, widget);

        const bool selected = opt.state & QStyle::State_Selected;
        const QColor fg = opt.palette.color(selected && !Lieve::Theme::styled() ? QPalette::HighlightedText : QPalette::Text);
        QColor dim = fg;
        dim.setAlphaF(0.6f);
        const QColor accent = selected ? fg : opt.palette.color(QPalette::Highlight);

        const QRect r = opt.rect.adjusted(10, 7, -12, -7);
        icon.paint(p, QRect(r.left(), r.center().y() - 12, 24, 24));
        const int left = r.left() + 36;

        QFont bold = opt.font;
        bold.setBold(true);
        const QFontMetrics fmBold(bold);
        const QFontMetrics fm(opt.font);
        const int line1 = r.top() + fmBold.ascent();
        const int line2 = r.bottom() - fm.descent();

        p->save();
        // Right column
        const bool open = index.data(OpenRole).toBool();
        const QString right1 = open ? QCoreApplication::translate("ProjectList", "● open") : index.data(TimeRole).toString();
        const QString right2 = index.data(BranchRole).toString();
        const int rightWidth = std::max(fm.horizontalAdvance(right1), fm.horizontalAdvance(right2));
        p->setFont(opt.font);
        p->setPen(open ? accent : dim);
        p->drawText(r.right() - fm.horizontalAdvance(right1), line1, right1);
        p->setPen(dim);
        p->drawText(r.right() - fm.horizontalAdvance(right2), line2, right2);

        // Name (+ pin) and path
        const int textRight = r.right() - rightWidth - 16;
        p->setFont(bold);
        p->setPen(fg);
        const QString shownName = fmBold.elidedText(name, Qt::ElideRight, textRight - left);
        p->drawText(left, line1, shownName);
        if (index.data(PinnedRole).toBool()) {
            const QIcon pin = QIcon::fromTheme(QStringLiteral("pin"), QIcon::fromTheme(QStringLiteral("window-pin")));
            const int x = left + fmBold.horizontalAdvance(shownName) + 6;
            if (x + 14 < textRight) {
                pin.paint(p, QRect(x, line1 - fmBold.ascent() + 1, 14, 14));
            }
        }
        p->setFont(opt.font);
        p->setPen(dim);
        p->drawText(left, line2, fm.elidedText(index.data(DirRole).toString(), Qt::ElideMiddle, textRight - left));
        p->restore();
    }
};
}

ProjectList::ProjectList(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Search projects…"));
    m_filter->setClearButtonEnabled(true);
    m_list = new QListWidget(this);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setUniformItemSizes(true);
    m_list->setIconSize(QSize(24, 24));
    m_list->setItemDelegate(new ProjectDelegate(m_list));
    m_list->setFocusPolicy(Qt::NoFocus); // typing always goes to the filter
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setMouseTracking(true);
    layout->addWidget(m_filter);
    layout->addWidget(m_list, 1);

    m_filter->installEventFilter(this);
    connect(m_filter, &QLineEdit::textChanged, this, &ProjectList::refilter);
    connect(m_list, &QListWidget::itemClicked, this, [this] {
        choose(QApplication::keyboardModifiers() & Qt::ControlModifier);
    });
    connect(m_list, &QListWidget::customContextMenuRequested, this, &ProjectList::showMenu);
    connect(&WindowManager::instance(), &WindowManager::projectsChanged, this, [this] {
        if (isVisible()) {
            reload();
        }
    });
}

void ProjectList::reload()
{
    m_projects = Projects::all();
    refilter();
}

void ProjectList::focusFilter()
{
    m_filter->setFocus();
    m_filter->selectAll();
}

void ProjectList::clearFilter()
{
    m_filter->clear();
}

void ProjectList::setPlaceholderText(const QString &text)
{
    m_filter->setPlaceholderText(text);
}

void ProjectList::setCurrentProject(const QString &path)
{
    m_current = path;
}

void ProjectList::refilter()
{
    QString query = m_filter->text().trimmed().toLower();
    query.remove(QLatin1Char(' '));
    const QSet<QString> open = WindowManager::instance().openProjects();

    QList<QPair<int, qsizetype>> order;
    for (qsizetype i = 0; i < m_projects.size(); ++i) {
        if (query.isEmpty()) {
            order.append({0, i});
            continue;
        }
        const QString pretty = prettyPath(m_projects.at(i).path);
        const int score = fuzzyScore(pretty, pretty.toLower(), query);
        if (score != INT_MIN) {
            order.append({score, i});
        }
    }
    std::stable_sort(order.begin(), order.end(), [](const auto &a, const auto &b) {
        return a.first > b.first;
    });

    m_list->clear();
    int mostRecentRow = 0;
    qint64 mostRecent = -1;
    for (const auto &[score, i] : std::as_const(order)) {
        const Project &project = m_projects.at(i);
        const bool repo = !project.branch.isEmpty();
        auto *item = new QListWidgetItem(QIcon::fromTheme(repo ? QStringLiteral("folder-git") : QStringLiteral("folder"),
                                                          QIcon::fromTheme(QStringLiteral("folder"))),
                                         project.name,
                                         m_list);
        item->setData(PathRole, project.path);
        item->setData(DirRole, prettyPath(project.path));
        item->setData(BranchRole, project.branch);
        item->setData(TimeRole, relativeTime(project.lastOpened));
        item->setData(OpenRole, open.contains(project.path));
        item->setData(PinnedRole, project.pinned);
        item->setToolTip(project.path);
        if (project.lastOpened > mostRecent && project.path != m_current) {
            mostRecent = project.lastOpened;
            mostRecentRow = m_list->count() - 1;
        }
    }
    // Empty filter: preselect the project used last, so Enter reopens it.
    m_list->setCurrentRow(query.isEmpty() ? mostRecentRow : 0);
}

void ProjectList::choose(bool alternate)
{
    if (QListWidgetItem *item = m_list->currentItem()) {
        Q_EMIT projectChosen(item->data(PathRole).toString(), alternate);
    }
}

void ProjectList::showMenu(const QPoint &pos)
{
    QListWidgetItem *item = m_list->itemAt(pos);
    if (!item) {
        return;
    }
    m_list->setCurrentItem(item);
    const QString path = item->data(PathRole).toString();
    const bool pinned = item->data(PinnedRole).toBool();
    const bool recent = pinned || !item->data(TimeRole).toString().isEmpty();

    QMenu menu(this);
    menu.addAction(QIcon::fromTheme(QStringLiteral("document-open")), tr("Open"), this, [this, path] {
        Q_EMIT projectChosen(path, false);
    });
    menu.addSeparator();
    menu.addAction(QIcon::fromTheme(pinned ? QStringLiteral("window-unpin") : QStringLiteral("window-pin")),
                   pinned ? tr("Unpin") : tr("Pin to Top"),
                   this,
                   [this, path, pinned] {
                       Projects::setPinned(path, !pinned);
                       JumpList::update();
                       reload();
                   });
    if (recent) {
        menu.addAction(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove from Recent"), this, [this, path] {
            Projects::forget(path);
            JumpList::update();
            reload();
        });
    }
    menu.addSeparator();
    menu.addAction(QIcon::fromTheme(QStringLiteral("edit-copy")), tr("Copy Path"), this, [path] {
        QApplication::clipboard()->setText(path);
    });
    menu.addAction(QIcon::fromTheme(QStringLiteral("system-file-manager")), tr("Reveal in File Manager"), this, [path] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}

bool ProjectList::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_filter && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        const int row = m_list->currentRow();
        const int page = std::max(1, m_list->viewport()->height() / std::max(1, m_list->sizeHintForRow(0)));
        switch (key->key()) {
        case Qt::Key_Escape:
            Q_EMIT cancelled();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            choose(key->modifiers() & Qt::ControlModifier);
            return true;
        case Qt::Key_Down:
            m_list->setCurrentRow(std::min(row + 1, m_list->count() - 1));
            return true;
        case Qt::Key_Up:
            m_list->setCurrentRow(std::max(row - 1, 0));
            return true;
        case Qt::Key_PageDown:
            m_list->setCurrentRow(std::min(row + page, m_list->count() - 1));
            return true;
        case Qt::Key_PageUp:
            m_list->setCurrentRow(std::max(row - page, 0));
            return true;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}
