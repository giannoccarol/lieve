#include "breadcrumbs.h"

#include "lsp.h"
#include "outline.h"
#include "settings.h"
#include "tabview.h"
#include "theme.h"
#include "vscodeicons.h"

#include <KTextEditor/Document>
#include <KTextEditor/View>

#include <QApplication>
#include <QCollator>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

using namespace Lieve;

namespace
{
constexpr int kHeight = 22;
constexpr int kIconSize = 16;
constexpr int kIconGap = 5;
constexpr int kSeparatorWidth = 20; // the chevron between two parts
constexpr int kMaxTextLength = 48;
constexpr int kMaxEntries = 2000; // in a list
constexpr int kVisibleRows = 14;

enum ItemRole {
    TypeRole = Qt::UserRole + 50,
    TargetRole, // folder or file path, or scope index
    FilledRole, // a folder's entries are listed
};
enum ItemType {
    FolderItem,
    FileItem,
    SymbolItem,
};

QString shortened(const QString &text)
{
    return text.size() > kMaxTextLength ? text.left(kMaxTextLength - 1) + QChar(0x2026) : text;
}

// A folder's entries as the Explorer lists them: folders first, without the excluded names.
QFileInfoList folderEntries(const QString &dir)
{
    const QStringList excluded = config()
                                     .value(QStringLiteral("explorer/exclude"),
                                            QStringList{QStringLiteral(".git"),
                                                        QStringLiteral(".svn"),
                                                        QStringLiteral(".hg"),
                                                        QStringLiteral("CVS"),
                                                        QStringLiteral(".DS_Store"),
                                                        QStringLiteral("Thumbs.db")})
                                     .toStringList();
    QFileInfoList entries = QDir(dir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::NoSort);
    entries.removeIf([&excluded](const QFileInfo &info) {
        const QString name = info.fileName();
        return name.endsWith(QLatin1String(".kate-swp")) || std::any_of(excluded.begin(), excluded.end(), [&name](const QString &e) {
                   return e.trimmed() == name;
               });
    });
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(entries.begin(), entries.end(), [&collator](const QFileInfo &a, const QFileInfo &b) {
        if (a.isDir() != b.isDir()) {
            return a.isDir();
        }
        return collator.compare(a.fileName(), b.fileName()) < 0;
    });
    if (entries.size() > kMaxEntries) {
        entries.resize(kMaxEntries);
    }
    return entries;
}

// The chevron between two parts: the shape of the tree's arrows.
void drawSeparator(QPainter &p, const QRectF &box, const QColor &color)
{
    QPainterPath path;
    const QPointF c = box.center();
    path.moveTo(c.x() - 2, c.y() - 4);
    path.lineTo(c.x() + 2, c.y());
    path.lineTo(c.x() - 2, c.y() + 4);
    QPen pen(color, 1.3);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.strokePath(path, pen);
    p.restore();
}
}

// The list a part of the breadcrumbs opens, below it: Quick Open's look.
class BreadcrumbPicker : public QFrame
{
public:
    explicit BreadcrumbPicker(QWidget *parent)
        : QFrame(parent)
    {
        setObjectName(QStringLiteral("overlay"));
        setFrameShape(QFrame::StyledPanel);
        auto *shadow = new QGraphicsDropShadowEffect(this);
        shadow->setBlurRadius(28);
        shadow->setOffset(0, 6);
        shadow->setColor(QColor(0, 0, 0, 140));
        setGraphicsEffect(shadow);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(4, 4, 4, 4);
        tree = new QTreeWidget(this);
        tree->setObjectName(QStringLiteral("breadcrumbList"));
        tree->setHeaderHidden(true);
        tree->setFrameShape(QFrame::NoFrame);
        tree->setUniformRowHeights(true);
        tree->setIconSize(QSize(kIconSize, kIconSize));
        tree->setIndentation(14);
        tree->setExpandsOnDoubleClick(false);
        tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        tree->setTextElideMode(Qt::ElideMiddle);
        layout->addWidget(tree);
        tree->installEventFilter(this);
        parent->installEventFilter(this);
        connect(tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
            if (activated) {
                activated(item);
            }
        });
        connect(tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
            if (expanded) {
                expanded(item, true);
            }
        });
        connect(tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
            if (expanded) {
                expanded(item, false);
            }
        });
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
            if (isVisible() && (!now || !isAncestorOf(now))) {
                dismiss(false);
            }
        });
        hide();
    }

    void popup(const QRect &anchor)
    {
        QWidget *p = parentWidget();
        int rows = tree->topLevelItemCount();
        int widest = 0;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            widest = std::max(widest, fontMetrics().horizontalAdvance(tree->topLevelItem(i)->text(0)));
        }
        bool nested = false;
        for (int i = 0; i < tree->topLevelItemCount() && !nested; ++i) {
            QTreeWidgetItem *item = tree->topLevelItem(i);
            nested = item->childCount() > 0 || item->childIndicatorPolicy() == QTreeWidgetItem::ShowIndicator;
        }
        tree->setRootIsDecorated(nested);
        rows = std::clamp(rows, 1, kVisibleRows);
        const int rowHeight = tree->sizeHintForRow(0) > 0 ? tree->sizeHintForRow(0) : fontMetrics().height() + 6;
        const int width = std::clamp(widest + kIconSize + 48 + (nested ? tree->indentation() : 0), 220, std::max(220, std::min(480, p->width() - 16)));
        const int height = rows * rowHeight + 2 * 4 + 2 * frameWidth();
        const int x = std::clamp(anchor.left() - 8, 8, std::max(8, p->width() - width - 8));
        setGeometry(x, anchor.bottom() + 3, width, height);
        show();
        raise();
        tree->setFocus();
        if (tree->currentItem()) {
            tree->scrollToItem(tree->currentItem(), QAbstractItemView::PositionAtCenter);
        }
    }

    void dismiss(bool cancelled)
    {
        if (isHidden()) {
            return;
        }
        hide();
        if (closed) {
            closed(cancelled);
        }
    }

    QTreeWidget *tree = nullptr;
    std::function<void(QTreeWidgetItem *)> activated;
    std::function<void(QTreeWidgetItem *, bool)> expanded;
    std::function<void(bool cancelled)> closed;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == tree && event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Escape) {
                dismiss(true);
                return true;
            }
            if ((key == Qt::Key_Return || key == Qt::Key_Enter) && tree->currentItem() && activated) {
                activated(tree->currentItem());
                return true;
            }
        }
        if (watched == parentWidget() && event->type() == QEvent::Resize) {
            dismiss(false);
        }
        return QFrame::eventFilter(watched, event);
    }
};

// ---------------------------------------------------------------------------

BreadcrumbBar::BreadcrumbBar(TabView *group,
                             DocumentScopes *scopes,
                             const std::function<QString()> &root,
                             const std::function<void(const QString &)> &open)
    : QWidget(group)
    , m_group(group)
    , m_scopes(scopes)
    , m_root(root)
    , m_openFile(open)
{
    setObjectName(QStringLiteral("breadcrumbs"));
    setFixedHeight(kHeight);
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &BreadcrumbBar::rebuild);
    connect(group, &TabView::currentChanged, this, [this] {
        setView(qobject_cast<KTextEditor::View *>(m_group->currentWidget()));
    });
    connect(scopes, &DocumentScopes::changed, this, [this](KTextEditor::Document *doc) {
        if (m_view && m_view->document() == doc) {
            schedule(0);
        }
    });
    group->setBarBelowTabs(this);
    setView(qobject_cast<KTextEditor::View *>(group->currentWidget()));
}

BreadcrumbBar::~BreadcrumbBar()
{
    delete m_picker.data();
}

void BreadcrumbBar::setView(KTextEditor::View *view)
{
    for (const QMetaObject::Connection &connection : std::as_const(m_connections)) {
        disconnect(connection);
    }
    m_connections.clear();
    closeList();
    m_view = view;
    setVisible(view != nullptr); // not above pages other than editors
    if (!view) {
        return;
    }
    KTextEditor::Document *doc = view->document();
    m_connections << connect(view, &KTextEditor::View::cursorPositionChanged, this, [this] {
        // Moving: shown at once; typing: once it pauses.
        const bool edited = m_view && m_view->document()->revision() != m_revision;
        if (edited) {
            m_timer.start(250);
        } else {
            schedule(60);
        }
    });
    m_connections << connect(doc, &KTextEditor::Document::documentUrlChanged, this, [this] {
        schedule(0);
    });
    m_connections << connect(doc, &KTextEditor::Document::highlightingModeChanged, this, [this] {
        schedule(0);
    });
    m_dirty = true;
    if (isVisible()) {
        rebuild();
    }
}

void BreadcrumbBar::schedule(int ms)
{
    if (!m_timer.isActive() || m_timer.remainingTime() > ms) {
        m_timer.start(ms);
    }
}

void BreadcrumbBar::rebuild()
{
    if (!isVisible()) {
        m_dirty = true;
        return;
    }
    if (m_picker && m_picker->isVisible()) {
        m_dirty = true; // after the list closes
        return;
    }
    m_dirty = false;
    m_segments.clear();
    m_tree.reset();
    m_hovered = -1;
    KTextEditor::View *view = m_view;
    if (!view) {
        update();
        return;
    }
    KTextEditor::Document *doc = view->document();
    m_revision = doc->revision();
    const QString path = doc->url().isLocalFile() ? doc->url().toLocalFile() : QString();
    const bool diff = doc->property("lieveDiffKey").isValid();
    if (!path.isEmpty() && !diff) {
        // The path from the project's folder, else from the home folder.
        const QString root = m_root ? m_root() : QString();
        const QString home = QDir::homePath();
        QString dir;
        QStringList parts;
        if (!root.isEmpty() && path.startsWith(root + QLatin1Char('/'))) {
            dir = root;
            parts = path.mid(root.size() + 1).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        } else if (path.startsWith(home + QLatin1Char('/'))) {
            dir = home;
            parts = path.mid(home.size() + 1).split(QLatin1Char('/'), Qt::SkipEmptyParts);
            m_segments.append({Segment::Folder, QStringLiteral("~"), QIcon(), home});
        } else {
            parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            m_segments.append({Segment::Folder, QStringLiteral("/"), QIcon(), QStringLiteral("/")});
        }
        for (qsizetype i = 0; i + 1 < parts.size(); ++i) {
            dir = (dir == QLatin1String("/") ? QString() : dir) + QLatin1Char('/') + parts.at(i);
            m_segments.append({Segment::Folder, parts.at(i), QIcon(), dir});
        }
        const QString name = QFileInfo(path).fileName();
        m_segments.append({Segment::File, name, VsCodeIcons::instance().fileIcon(name), path});
    } else {
        // Untitled and diff tabs: their title.
        const QString custom = doc->property("lieveTitle").toString();
        const QString title = custom.isEmpty() ? doc->documentName() : custom;
        const QString iconName = doc->property("lieveIconName").toString();
        m_segments.append({Segment::File, title, VsCodeIcons::instance().fileIcon(iconName.isEmpty() ? title : iconName), QString()});
    }
    if (!diff) {
        m_tree = m_scopes->scopes(doc);
        if (m_tree) {
            const KTextEditor::Cursor cursor = view->cursorPosition();
            const QList<int> chain = m_tree->chainAt(cursor.line(), cursor.column());
            for (const int i : chain) {
                if (m_tree->isSymbol(i)) {
                    const Scope &s = m_tree->scopes[i];
                    Segment segment{Segment::Symbol, shortened(s.name), symbolKindIcon(s.kind), QString()};
                    segment.scope = i;
                    m_segments.append(segment);
                }
            }
        }
    }
    layoutSegments();
    update();
}

void BreadcrumbBar::layoutSegments()
{
    const QFontMetrics fm(fontMetrics());
    const int left = Theme::styled() ? 12 : 8; // under the first tab's icon
    const auto widthOf = [&fm](const Segment &s) {
        return (s.icon.isNull() ? 0 : kIconSize + kIconGap) + fm.horizontalAdvance(s.text);
    };
    // Too long for the bar: the first folders give way to "…".
    for (Segment &s : m_segments) {
        s.shown = true;
    }
    const auto total = [&] {
        int width = left;
        bool first = true;
        bool hidden = false;
        for (const Segment &s : std::as_const(m_segments)) {
            if (!s.shown) {
                hidden = true;
                continue;
            }
            width += (first ? 0 : kSeparatorWidth) + widthOf(s);
            first = false;
        }
        return width + (hidden ? fm.horizontalAdvance(QChar(0x2026)) + kSeparatorWidth : 0) + 8;
    };
    for (Segment &s : m_segments) {
        if (total() <= width() || s.type != Segment::Folder) {
            break;
        }
        s.shown = false;
    }
    m_segments.removeIf([](const Segment &s) {
        return s.type == Segment::Ellipsis;
    });
    if (!m_segments.isEmpty() && !m_segments.first().shown) {
        m_segments.prepend({Segment::Ellipsis, QString(QChar(0x2026)), QIcon(), QString()});
    }
    int x = left;
    bool first = true;
    for (Segment &s : m_segments) {
        if (!s.shown) {
            s.rect = QRect();
            continue;
        }
        x += first ? 0 : kSeparatorWidth;
        first = false;
        s.rect = QRect(x, 0, widthOf(s), kHeight);
        x = s.rect.right() + 1;
    }
}

void BreadcrumbBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const bool styled = Theme::styled();
    const Colors &c = Theme::colors();
    p.fillRect(rect(), styled ? c.editor : palette().color(QPalette::Window));
    const QColor normal = styled ? c.dim : palette().color(QPalette::PlaceholderText);
    const QColor strong = styled ? c.fg : palette().color(QPalette::WindowText);
    bool first = true;
    for (int i = 0; i < m_segments.size(); ++i) {
        const Segment &s = m_segments.at(i);
        if (!s.shown) {
            continue;
        }
        if (!first) {
            drawSeparator(p, QRectF(s.rect.left() - kSeparatorWidth, 0, kSeparatorWidth, kHeight), normal);
        }
        first = false;
        QRect r = s.rect;
        if (!s.icon.isNull()) {
            s.icon.paint(&p, QRect(r.left(), (kHeight - kIconSize) / 2, kIconSize, kIconSize));
            r.setLeft(r.left() + kIconSize + kIconGap);
        }
        p.setPen(i == m_hovered || i == m_open ? strong : normal);
        p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, s.text);
    }
}

int BreadcrumbBar::segmentAt(const QPoint &pos) const
{
    for (int i = 0; i < m_segments.size(); ++i) {
        const Segment &s = m_segments.at(i);
        if (s.shown && s.type != Segment::Ellipsis && s.rect.adjusted(-3, 0, 3, 0).contains(pos)) {
            return i;
        }
    }
    return -1;
}

void BreadcrumbBar::mouseMoveEvent(QMouseEvent *event)
{
    const int hovered = segmentAt(event->position().toPoint());
    if (hovered != m_hovered) {
        m_hovered = hovered;
        update();
    }
}

void BreadcrumbBar::leaveEvent(QEvent *)
{
    if (m_hovered >= 0) {
        m_hovered = -1;
        update();
    }
}

void BreadcrumbBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const int index = segmentAt(event->position().toPoint());
    if (index >= 0 && index == m_open && m_picker && m_picker->isVisible()) {
        closeList(); // a second click closes it
        if (m_view) {
            m_view->setFocus();
        }
        return;
    }
    if (index >= 0) {
        showList(index);
    }
}

void BreadcrumbBar::resizeEvent(QResizeEvent *)
{
    layoutSegments();
}

void BreadcrumbBar::showEvent(QShowEvent *)
{
    if (m_dirty) {
        rebuild();
    }
}

void BreadcrumbBar::showList(int index)
{
    if (index < 0 || index >= m_segments.size() || !m_view) {
        return;
    }
    auto *main = qobject_cast<QMainWindow *>(window());
    QWidget *parent = main && main->centralWidget() ? main->centralWidget() : window();
    if (!m_picker) {
        m_picker = new BreadcrumbPicker(parent);
        m_picker->closed = [this](bool cancelled) {
            m_open = -1;
            update();
            if (cancelled && m_view) {
                m_view->setFocus();
            }
            if (m_dirty) {
                schedule(0);
            }
        };
        m_picker->expanded = [this](QTreeWidgetItem *item, bool open) {
            if (item->data(0, TypeRole).toInt() != FolderItem) {
                return;
            }
            item->setIcon(0, VsCodeIcons::instance().folderIcon(item->text(0), open));
            if (open && !item->data(0, FilledRole).toBool()) {
                item->setData(0, FilledRole, true);
                const QFileInfoList entries = folderEntries(item->data(0, TargetRole).toString());
                for (const QFileInfo &info : entries) {
                    auto *child = new QTreeWidgetItem(item, {info.fileName()});
                    child->setData(0, TypeRole, info.isDir() ? FolderItem : FileItem);
                    child->setData(0, TargetRole, info.absoluteFilePath());
                    child->setIcon(0, info.isDir() ? VsCodeIcons::instance().folderIcon(info.fileName(), false) : VsCodeIcons::instance().fileIcon(info.fileName()));
                    if (info.isDir()) {
                        child->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
                    }
                }
                item->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicatorWhenChildless);
            }
        };
        m_picker->activated = [this](QTreeWidgetItem *item) {
            const int type = item->data(0, TypeRole).toInt();
            if (type == FolderItem) {
                item->setExpanded(!item->isExpanded());
                return;
            }
            QPointer<KTextEditor::View> view = m_view;
            const std::shared_ptr<const ScopeTree> tree = m_tree;
            const QString target = item->data(0, TargetRole).toString();
            const int scope = item->data(0, TargetRole).toInt();
            closeList();
            if (!view) {
                return;
            }
            view->setFocus(); // this group becomes the active one
            if (type == FileItem) {
                if (m_openFile) {
                    m_openFile(target);
                }
            } else if (tree && scope >= 0 && scope < int(tree->scopes.size())) {
                const Scope &s = tree->scopes[scope];
                view->setCursorPosition(KTextEditor::Cursor(s.targetLine, s.targetColumn));
            }
        };
    }
    QTreeWidget *list = m_picker->tree;
    list->clear();
    const Segment &segment = m_segments.at(index);
    QTreeWidgetItem *current = nullptr;
    if (segment.type == Segment::Folder) {
        // The folder's files; the next part of the path is selected.
        QString next;
        for (int i = index + 1; i < m_segments.size() && next.isEmpty(); ++i) {
            next = m_segments.at(i).path;
        }
        const QFileInfoList entries = folderEntries(segment.path);
        for (const QFileInfo &info : entries) {
            auto *item = new QTreeWidgetItem(list, {info.fileName()});
            item->setData(0, TypeRole, info.isDir() ? FolderItem : FileItem);
            item->setData(0, TargetRole, info.absoluteFilePath());
            item->setIcon(0, info.isDir() ? VsCodeIcons::instance().folderIcon(info.fileName(), false) : VsCodeIcons::instance().fileIcon(info.fileName()));
            if (info.isDir()) {
                item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
            }
            if (!current && !next.isEmpty() && (info.absoluteFilePath() == next || next.startsWith(info.absoluteFilePath() + QLatin1Char('/')))) {
                current = item;
            }
        }
    } else if (m_tree) {
        // The symbols at that level: the file's top level, or the symbol's siblings.
        int parent = -1;
        for (int i = index - 1; i >= 0 && segment.type == Segment::Symbol; --i) {
            if (m_segments.at(i).type == Segment::Symbol) {
                parent = m_segments.at(i).scope;
                break;
            }
        }
        int selected = segment.type == Segment::Symbol ? segment.scope : -1;
        if (segment.type == Segment::File) {
            for (const Segment &s : std::as_const(m_segments)) {
                if (s.type == Segment::Symbol) {
                    selected = s.scope;
                    break;
                }
            }
        }
        int count = 0;
        std::function<void(QTreeWidgetItem *, int, int)> add = [&](QTreeWidgetItem *under, int scope, int depth) {
            const QList<int> symbols = m_tree->symbolsUnder(scope);
            for (const int i : symbols) {
                if (++count > kMaxEntries) {
                    return;
                }
                const Scope &s = m_tree->scopes[i];
                auto *item = under ? new QTreeWidgetItem(under, {s.name}) : new QTreeWidgetItem(list, {s.name});
                item->setData(0, TypeRole, SymbolItem);
                item->setData(0, TargetRole, i);
                item->setIcon(0, symbolKindIcon(s.kind));
                item->setToolTip(0, s.name);
                if (i == selected) {
                    current = item;
                }
                if (depth < 6) {
                    add(item, i, depth + 1);
                }
            }
        };
        add(nullptr, parent, 0);
    }
    if (list->topLevelItemCount() == 0) {
        return;
    }
    list->setCurrentItem(current ? current : list->topLevelItem(0));
    m_open = index;
    m_hovered = -1;
    update();
    m_picker->popup(QRect(mapTo(parent, segment.rect.topLeft()), QSize(segment.rect.width(), height() - segment.rect.top())));
}

void BreadcrumbBar::closeList()
{
    if (m_picker && m_picker->isVisible()) {
        m_picker->hide();
    }
    if (m_open >= 0) {
        m_open = -1;
        update();
    }
    if (m_dirty) {
        schedule(0);
    }
}

QStringList BreadcrumbBar::texts() const
{
    QStringList texts;
    for (const Segment &s : m_segments) {
        if (s.shown) {
            texts << s.text;
        }
    }
    return texts;
}

bool BreadcrumbBar::openList(int index)
{
    QList<int> shown;
    for (int i = 0; i < m_segments.size(); ++i) {
        if (m_segments.at(i).shown) {
            shown << i;
        }
    }
    if (index < 0) {
        index += int(shown.size());
    }
    if (index < 0 || index >= shown.size()) {
        return false;
    }
    showList(shown.at(index));
    return m_picker && m_picker->isVisible();
}

QStringList BreadcrumbBar::listRows() const
{
    QStringList rows;
    if (!m_picker || !m_picker->isVisible()) {
        return rows;
    }
    QTreeWidget *list = m_picker->tree;
    for (QTreeWidgetItemIterator it(list); *it; ++it) {
        int depth = 0;
        for (QTreeWidgetItem *p = (*it)->parent(); p; p = p->parent()) {
            if (!p->isExpanded()) {
                depth = -1;
                break;
            }
            ++depth;
        }
        if (depth >= 0) {
            rows << QString(depth * 2, QLatin1Char(' ')) + ((*it) == list->currentItem() ? QStringLiteral("*") : QString()) + (*it)->text(0);
        }
    }
    return rows;
}

bool BreadcrumbBar::activateListRow(const QString &text)
{
    if (!m_picker || !m_picker->isVisible()) {
        return false;
    }
    for (QTreeWidgetItemIterator it(m_picker->tree); *it; ++it) {
        if ((*it)->text(0) == text) {
            m_picker->tree->setCurrentItem(*it);
            m_picker->activated(*it);
            return true;
        }
    }
    return false;
}
