#include "problemspanel.h"

#include "delegates.h"
#include "roundedrows.h"
#include "theme.h"
#include "vscodeicons.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QMap>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QShortcut>
#include <QTextDocument>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <tuple>

using namespace Lieve;
using Problem = LspManager::Problem;

namespace
{
constexpr int ProblemItem = 16; // KindRole of a problem row (file rows are FileItem)
constexpr int DetailRole = Qt::UserRole + 100; // "ts(2304) [Ln 12, Col 5]"
constexpr int MessageRole = Qt::UserRole + 101; // the whole message

// Line icons like VS Code's (codicons), in the colours of the underlines:
// a circle with × for errors, a triangle with ! for warnings, a circle
// with i for information. `color` invalid = the severity's colour.
QPixmap paintSeverity(int severity, qreal dpr, QColor color)
{
    QPixmap pixmap(QSize(16, 16) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    if (!color.isValid()) {
        color = diagnosticColor(severity);
    }
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(color, 1.2);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const auto dot = [&p, &color](QPointF at) {
        p.save();
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(at, 0.8, 0.8);
        p.restore();
    };
    if (severity == 2) {
        QPainterPath triangle;
        triangle.moveTo(8, 2.1);
        triangle.lineTo(14.3, 13.4);
        triangle.lineTo(1.7, 13.4);
        triangle.closeSubpath();
        p.drawPath(triangle);
        p.drawLine(QPointF(8, 6.3), QPointF(8, 9.3));
        dot(QPointF(8, 11.3));
    } else {
        p.drawEllipse(QPointF(8, 8), 6, 6);
        if (severity == 1) {
            p.drawLine(QPointF(5.9, 5.9), QPointF(10.1, 10.1));
            p.drawLine(QPointF(10.1, 5.9), QPointF(5.9, 10.1));
        } else {
            p.drawLine(QPointF(8, 7.4), QPointF(8, 10.9));
            dot(QPointF(8, 5.2));
        }
    }
    return pixmap;
}

QIcon severityIcon(int severity, const QColor &color = QColor())
{
    QIcon icon;
    icon.addPixmap(paintSeverity(severity, 1, color));
    icon.addPixmap(paintSeverity(severity, 2, color));
    return icon;
}

QString firstLine(const QString &text)
{
    return text.section(QLatin1Char('\n'), 0, 0).trimmed();
}

// File rows as in Search; problem rows: icon, message, then dim
// "source(code) [Ln, Col]".
class ProblemDelegate : public FileItemDelegate
{
public:
    using FileItemDelegate::FileItemDelegate;

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (index.data(KindRole).toInt() != ProblemItem) {
            FileItemDelegate::paint(p, option, index);
            return;
        }
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QString message = opt.text;
        opt.text.clear();
        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, p, widget); // background, selection, icon

        const QRect r = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget).adjusted(2, 0, -4, 0);
        const bool selected = opt.state & QStyle::State_Selected;
        const QPalette::ColorGroup group = (opt.state & QStyle::State_Active) ? QPalette::Active : QPalette::Inactive;
        const QColor fg = opt.palette.color(group, selected && !Theme::styled() ? QPalette::HighlightedText : QPalette::Text);
        QColor dim = fg;
        dim.setAlphaF(0.55f);
        const QFontMetrics fm(opt.font);
        const int baseline = r.top() + (r.height() - fm.height()) / 2 + fm.ascent();
        const QString detail = index.data(DetailRole).toString();
        constexpr int gap = 8;

        // The message gives way to the place in the file, down to half the row.
        const int room = std::max(r.width() - fm.horizontalAdvance(detail) - gap, r.width() / 2);
        const QString shown = fm.elidedText(message, Qt::ElideRight, std::max(0, room));
        p->save();
        p->setFont(opt.font);
        p->setClipRect(r);
        p->setPen(fg);
        p->drawText(r.left(), baseline, shown);
        const int x = r.left() + fm.horizontalAdvance(shown) + gap;
        if (x < r.right()) {
            p->setPen(dim);
            p->drawText(x, baseline, fm.elidedText(detail, Qt::ElideRight, r.right() - x));
        }
        p->restore();
    }
};

// Identifies a row across rebuilds (the current row stays current).
QString rowKey(const QTreeWidgetItem *item)
{
    if (!item) {
        return {};
    }
    const QString path = item->data(0, PathRole).toString();
    if (item->data(0, KindRole).toInt() == FileItem) {
        return path;
    }
    return QStringList{path, item->data(0, LineRole).toString(), item->data(0, ColumnRole).toString(), item->data(0, MessageRole).toString()}.join(
        QLatin1Char('\n'));
}
}

ProblemsPanel::ProblemsPanel(const Source &problems, const std::function<QString()> &root, QWidget *parent)
    : QWidget(parent)
    , m_problems(problems)
    , m_root(root)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 0);
    layout->setSpacing(0);

    m_empty = new QLabel(tr("No problems have been detected in the workspace."), this);
    m_empty->setWordWrap(true);
    m_empty->setContentsMargins(6, 4, 6, 4);
    m_empty->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    layout->addWidget(m_empty);

    m_tree = new RoundedTree<QTreeWidget>(this);
    m_tree->setObjectName(QStringLiteral("problemsTree"));
    m_tree->setHeaderHidden(true);
    m_tree->setColumnCount(1);
    m_tree->setUniformRowHeights(true);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setIndentation(14);
    m_tree->setIconSize(QSize(16, 16));
    m_tree->setExpandsOnDoubleClick(false);
    m_tree->setItemDelegate(new ProblemDelegate(m_tree));
    layout->addWidget(m_tree, 1);
    setFocusProxy(m_tree);

    connect(m_tree, &QTreeWidget::itemClicked, this, &ProblemsPanel::activate);
    for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
        auto *enter = new QShortcut(QKeySequence(key), m_tree, nullptr, nullptr, Qt::WidgetShortcut);
        connect(enter, &QShortcut::activated, this, [this] {
            activate(m_tree->currentItem());
        });
    }
    connect(m_tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
        m_collapsed.insert(item->data(0, PathRole).toString());
    });
    connect(m_tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        m_collapsed.remove(item->data(0, PathRole).toString());
    });
}

QIcon ProblemsPanel::tabIcon()
{
    QColor color = QApplication::palette().color(QPalette::WindowText);
    color.setAlphaF(0.8f);
    return severityIcon(2, color);
}

void ProblemsPanel::refresh()
{
    if (isVisible()) {
        rebuild();
    } else {
        m_stale = true;
    }
}

void ProblemsPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_stale) {
        rebuild();
    }
}

void ProblemsPanel::rebuild()
{
    m_stale = false;
    const QString root = m_root ? m_root() : QString();
    QMap<QString, QList<Problem>> byFile;
    const QList<Problem> problems = m_problems ? m_problems() : QList<Problem>();
    for (const Problem &problem : problems) {
        byFile[problem.path].append(problem);
    }
    // Errors first, then by place; files with errors first, then by path (like VS Code).
    struct File {
        QString path;
        QString relative;
        QList<Problem> problems;
    };
    QList<File> files;
    for (auto it = byFile.begin(); it != byFile.end(); ++it) {
        File file{it.key(), root.isEmpty() ? it.key() : QDir(root).relativeFilePath(it.key()), it.value()};
        std::sort(file.problems.begin(), file.problems.end(), [](const Problem &a, const Problem &b) {
            return std::tie(a.severity, a.line, a.column) < std::tie(b.severity, b.line, b.column);
        });
        files.append(file);
    }
    std::sort(files.begin(), files.end(), [](const File &a, const File &b) {
        const int sa = a.problems.first().severity;
        const int sb = b.problems.first().severity;
        if (sa != sb) {
            return sa < sb;
        }
        return a.relative.compare(b.relative, Qt::CaseInsensitive) < 0;
    });

    const QString current = rowKey(m_tree->currentItem());
    const int scroll = m_tree->verticalScrollBar()->value();
    static const QIcon icons[] = {severityIcon(1), severityIcon(2), severityIcon(3)};
    m_tree->setUpdatesEnabled(false);
    m_tree->clear();
    QTreeWidgetItem *restore = nullptr;
    for (const File &file : std::as_const(files)) {
        const QFileInfo info(file.relative);
        auto *fileItem = new QTreeWidgetItem(m_tree);
        fileItem->setText(0, info.fileName());
        fileItem->setIcon(0, VsCodeIcons::instance().fileIcon(info.fileName()));
        fileItem->setToolTip(0, file.relative);
        fileItem->setData(0, KindRole, FileItem);
        fileItem->setData(0, PathRole, file.path);
        fileItem->setData(0, DirRole, info.path() == QLatin1String(".") ? QString() : info.path());
        fileItem->setData(0, CountRole, int(file.problems.size()));
        if (rowKey(fileItem) == current) {
            restore = fileItem;
        }
        for (const Problem &problem : file.problems) {
            auto *item = new QTreeWidgetItem(fileItem);
            QString origin = problem.source;
            if (!problem.code.isEmpty()) {
                origin += QStringLiteral("(%1)").arg(problem.code);
            }
            const QString place = tr("[Ln %1, Col %2]").arg(problem.line + 1).arg(problem.column + 1);
            item->setText(0, firstLine(problem.message));
            item->setIcon(0, icons[std::clamp(problem.severity, 1, 3) - 1]);
            item->setData(0, KindRole, ProblemItem);
            item->setData(0, PathRole, file.path);
            item->setData(0, LineRole, problem.line);
            item->setData(0, ColumnRole, problem.column);
            item->setData(0, MessageRole, problem.message);
            item->setData(0, DetailRole, origin.isEmpty() ? place : origin + QLatin1Char(' ') + place);
            item->setToolTip(0, Qt::convertFromPlainText(origin.isEmpty() ? problem.message : problem.message + QStringLiteral("\n") + origin));
            if (!restore && rowKey(item) == current) {
                restore = item;
            }
        }
        fileItem->setExpanded(!m_collapsed.contains(file.path));
    }
    if (restore) {
        m_tree->setCurrentItem(restore);
    }
    m_tree->verticalScrollBar()->setValue(scroll);
    m_tree->setUpdatesEnabled(true);
    // The empty list stays below the message: it keeps the focus (Ctrl+Shift+M hides the panel again).
    m_empty->setVisible(files.isEmpty());
}

void ProblemsPanel::activate(QTreeWidgetItem *item)
{
    if (!item) {
        return;
    }
    if (item->data(0, KindRole).toInt() == FileItem) {
        item->setExpanded(!item->isExpanded());
        return;
    }
    Q_EMIT problemActivated(item->data(0, PathRole).toString(), item->data(0, LineRole).toInt(), item->data(0, ColumnRole).toInt());
}
